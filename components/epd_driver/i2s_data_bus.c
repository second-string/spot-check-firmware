#include "i2s_data_bus.h"
#include "display_ops.h"
#include "driver/periph_ctrl.h"
#if ESP_IDF_VERSION < (4, 0, 0) || ARDUINO_ARCH_ESP32
#include "rom/lldesc.h"
#include "rom/gpio.h"
#else
#include "esp32/rom/lldesc.h"
#include "esp32/rom/gpio.h"
#endif
#include "esp_heap_caps.h"
#include "soc/i2s_reg.h"
#include "soc/i2s_struct.h"
#include "soc/rtc.h"

/// DMA descriptors for front and back line buffer.
/// We use two buffers, so one can be filled while the other
/// is transmitted.
typedef struct {
  volatile lldesc_t *dma_desc_a;
  volatile lldesc_t *dma_desc_b;

  /// Front and back line buffer.
  uint8_t *buf_a;
  uint8_t *buf_b;
} i2s_parallel_state_t;

/// Indicates which line buffer is currently back / front.
static int current_buffer = 0;

/// The I2S state instance.
static i2s_parallel_state_t i2s_state;

static intr_handle_t gI2S_intr_handle = NULL;

/// Indicates the device has finished its transmission and is ready again.
static volatile bool output_done = true;
/// The start pulse pin extracted from the configuration for use in the "done"
/// interrupt.
static gpio_num_t start_pulse_pin;

/// Initializes a DMA descriptor.
static void fill_dma_desc(volatile lldesc_t *dmadesc, uint8_t *buf,
                          uint32_t epd_row_width) {
  dmadesc->size = epd_row_width / 4;
  dmadesc->length = epd_row_width / 4;
  dmadesc->buf = buf;
  dmadesc->eof = 1;
  dmadesc->sosf = 1;
  dmadesc->owner = 1;
  dmadesc->qe.stqe_next = 0;
  dmadesc->offset = 0;
}

/// Address of the currently front DMA descriptor,
/// which uses only the lower 20bits (according to TRM)
uint32_t dma_desc_addr() {
  return (uint32_t)(current_buffer ? i2s_state.dma_desc_a
                                   : i2s_state.dma_desc_b) &
         0x000FFFFF;
}

/// Set up a GPIO as output and route it to a signal.
static void gpio_setup_out(int gpio, int sig, bool invert) {
  if (gpio == -1)
    return;
  PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[gpio], PIN_FUNC_GPIO);
  gpio_set_direction(gpio, GPIO_MODE_DEF_OUTPUT);
  gpio_matrix_out(gpio, sig, invert, false);
}

/// Resets "Start Pulse" signal when the current row output is done.
static void IRAM_ATTR i2s_int_hdl(void *arg) {
  i2s_dev_t *dev = &I2S1;
  if (dev->int_st.out_done) {
    //gpio_set_level(start_pulse_pin, 1);
    //gpio_set_level(GPIO_NUM_26, 0);
    output_done = true;
  }
  // Clear the interrupt. Otherwise, the whole device would hang.
  dev->int_clr.val = dev->int_raw.val;
}

volatile uint8_t IRAM_ATTR *i2s_get_current_buffer() {
  return current_buffer ? i2s_state.dma_desc_a->buf : i2s_state.dma_desc_b->buf;
}

bool IRAM_ATTR i2s_is_busy() {
  // DMA and FIFO must be done
  return !output_done || !I2S1.state.tx_idle;
}

void IRAM_ATTR i2s_switch_buffer() {
  // either device is done transmitting or the switch must be away from the
  // buffer currently used by the DMA engine.
  while (i2s_is_busy() && dma_desc_addr() != I2S1.out_link.addr) {
  };
  current_buffer = !current_buffer;
}

void IRAM_ATTR i2s_start_line_output() {
  output_done = false;

  i2s_dev_t *dev = &I2S1;
  dev->conf.tx_start = 0;
  dev->conf.tx_reset = 1;
  dev->conf.tx_fifo_reset = 1;
  dev->conf.rx_fifo_reset = 1;
  dev->conf.tx_reset = 0;
  dev->conf.tx_fifo_reset = 0;
  dev->conf.rx_fifo_reset = 0;
  dev->out_link.addr = dma_desc_addr();
  dev->out_link.start = 1;

  // sth is pulled up through peripheral interrupt
  // This is timing-critical!
  gpio_set_level(start_pulse_pin, 0);
  dev->conf.tx_start = 1;
}

void i2s_gpio_attach() {
  gpio_num_t I2S_GPIO_BUS[] = {D6, D7, D4,
                               D5, D2, D3,
                               D0, D1};

  gpio_set_direction(STH, GPIO_MODE_OUTPUT);
  gpio_set_level(STH, 1);
  // Use I2S1 with no signal offset (for some reason the offset seems to be
  // needed in 16-bit mode, but not in 8 bit mode.
  int signal_base = I2S1O_DATA_OUT0_IDX;

  // Setup and route GPIOS
  for (int x = 0; x < 8; x++) {
    gpio_setup_out(I2S_GPIO_BUS[x], signal_base + x, false);
  }
  // Invert word select signal
  gpio_setup_out(CKH, I2S1O_WS_OUT_IDX, true);

}

void i2s_gpio_detach() {
  gpio_set_direction(D0, GPIO_MODE_INPUT);
  gpio_set_direction(D1, GPIO_MODE_INPUT);
  gpio_set_direction(D2, GPIO_MODE_INPUT);
  gpio_set_direction(D3, GPIO_MODE_INPUT);
  gpio_set_direction(D4, GPIO_MODE_INPUT);
  gpio_set_direction(D5, GPIO_MODE_INPUT);
  gpio_set_direction(D6, GPIO_MODE_INPUT);
  gpio_set_direction(D7, GPIO_MODE_INPUT);
  gpio_set_direction(STH, GPIO_MODE_INPUT);
  gpio_set_direction(CKH, GPIO_MODE_INPUT);
}

void i2s_bus_init(uint32_t epd_row_width) {
  i2s_gpio_attach();

  // store pin in global variable for use in interrupt.
  start_pulse_pin = STH;

  periph_module_enable(PERIPH_I2S1_MODULE);

  i2s_dev_t *dev = &I2S1;

  // Initialize device
  dev->conf.tx_reset = 1;
  dev->conf.tx_reset = 0;

  // Reset DMA
  dev->lc_conf.in_rst = 1;
  dev->lc_conf.in_rst = 0;
  dev->lc_conf.out_rst = 1;
  dev->lc_conf.out_rst = 0;

  // Setup I2S config. See section 12 of Technical Reference Manual
  // Enable LCD mode
  dev->conf2.val = 0;
  dev->conf2.lcd_en = 1;

  // Enable FRAME1-Mode (See technical reference manual)
  dev->conf2.lcd_tx_wrx2_en = 1;
  dev->conf2.lcd_tx_sdx2_en = 0;

  // Set to 8 bit parallel output
  dev->sample_rate_conf.val = 0;
  dev->sample_rate_conf.tx_bits_mod = 8;

  // Half speed of bit clock in LCD mode.
  // (Smallest possible divider according to the spec).
  dev->sample_rate_conf.tx_bck_div_num = 2;

#if defined(CONFIG_EPD_DISPLAY_TYPE_ED097OC4_LQ)
  // Initialize Audio Clock (APLL) for 120 Mhz.
  rtc_clk_apll_enable(true);
  rtc_clk_apll_coeff_set(0, 0, 0, 8);
#else
  // Initialize Audio Clock (APLL) for 100 Mhz.
  rtc_clk_apll_enable(true);
  rtc_clk_apll_coeff_set(0, 0, 0, 8);
#endif

  // Set Audio Clock Dividers
  dev->clkm_conf.val = 0;
  dev->clkm_conf.clka_en = 1;
  dev->clkm_conf.clkm_div_a = 1;
  dev->clkm_conf.clkm_div_b = 0;
  // 2 is the smallest possible divider according to the spec.
  dev->clkm_conf.clkm_div_num = 2;

  // Set up FIFO
  dev->fifo_conf.val = 0;
  dev->fifo_conf.tx_fifo_mod_force_en = 1;
  dev->fifo_conf.tx_fifo_mod = 1;
  dev->fifo_conf.tx_data_num = 32;
  dev->fifo_conf.dscr_en = 1;

  // Stop after transmission complete
  dev->conf1.val = 0;
  dev->conf1.tx_stop_en = 1;
  dev->conf1.tx_pcm_bypass = 1;

  // Configure TX channel
  dev->conf_chan.val = 0;
  dev->conf_chan.tx_chan_mod = 1;
  dev->conf.tx_right_first = 1;

  dev->timing.val = 0;

  // Allocate DMA descriptors
  i2s_state.buf_a = heap_caps_malloc(epd_row_width / 4, MALLOC_CAP_DMA);
  i2s_state.buf_b = heap_caps_malloc(epd_row_width / 4, MALLOC_CAP_DMA);
  i2s_state.dma_desc_a = heap_caps_malloc(sizeof(lldesc_t), MALLOC_CAP_DMA);
  i2s_state.dma_desc_b = heap_caps_malloc(sizeof(lldesc_t), MALLOC_CAP_DMA);

  // and fill them
  fill_dma_desc(i2s_state.dma_desc_a, i2s_state.buf_a, epd_row_width);
  fill_dma_desc(i2s_state.dma_desc_b, i2s_state.buf_b, epd_row_width);

  // enable "done" interrupt
  SET_PERI_REG_BITS(I2S_INT_ENA_REG(1), I2S_OUT_DONE_INT_ENA_V, 1,
                    I2S_OUT_DONE_INT_ENA_S);
  // register interrupt
  esp_intr_alloc(ETS_I2S1_INTR_SOURCE, 0, i2s_int_hdl, 0, &gI2S_intr_handle);

  // Reset FIFO/DMA
  dev->lc_conf.in_rst = 1;
  dev->lc_conf.out_rst = 1;
  dev->lc_conf.ahbm_rst = 1;
  dev->lc_conf.ahbm_fifo_rst = 1;
  dev->lc_conf.in_rst = 0;
  dev->lc_conf.out_rst = 0;
  dev->lc_conf.ahbm_rst = 0;
  dev->lc_conf.ahbm_fifo_rst = 0;
  dev->conf.tx_reset = 1;
  dev->conf.tx_fifo_reset = 1;
  dev->conf.rx_fifo_reset = 1;
  dev->conf.tx_reset = 0;
  dev->conf.tx_fifo_reset = 0;
  dev->conf.rx_fifo_reset = 0;

  // Start dma on front buffer
  dev->lc_conf.val =
      I2S_OUT_DATA_BURST_EN | I2S_OUTDSCR_BURST_EN | I2S_OUT_DATA_BURST_EN;
  dev->out_link.addr = ((uint32_t)(i2s_state.dma_desc_a));
  dev->out_link.start = 1;

  dev->int_clr.val = dev->int_raw.val;

  dev->int_ena.val = 0;
  dev->int_ena.out_done = 1;

  dev->conf.tx_start = 0;
}

void i2s_deinit() {
  esp_intr_free(gI2S_intr_handle);

  free(i2s_state.buf_a);
  free(i2s_state.buf_b);
  free((void *)i2s_state.dma_desc_a);
  free((void *)i2s_state.dma_desc_b);

  rtc_clk_apll_coeff_set(0, 0, 0, 8);
  rtc_clk_apll_enable(true);
  periph_module_disable(PERIPH_I2S1_MODULE);
}