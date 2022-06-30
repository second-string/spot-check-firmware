#include "constants.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "log.h"

#include "url_decode.h"

#define TAG "sc-url-decode"

void urldecode2(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a' - 'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';
            if (b >= 'a')
                b -= 'a' - 'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

/*
 * Splits a decoded querystring into an array of key/value Tuple pairs. Caller responsible
 * for allocating array of tuples passed in.
 */
esp_err_t get_key_values(char *decoded_str, Tuple *tuple_array, int num_tuples_available) {
    const char *initial_delim       = "&";
    char        inner_delim         = '=';
    int         current_tuple_index = 0;

    // strtok modifies string in place, copy to not bork callers pointer
    size_t input_str_len = strlen(decoded_str) + 1;
    char   temp_input_str[input_str_len];
    strncpy(temp_input_str, decoded_str, input_str_len);

    // Outer loops splits up key/value strings by & delimiter,
    // inner parses those strings into key/value tuples and copies
    // into the return Tuple array values
    char *next_key_value_str = strtok(temp_input_str, initial_delim);
    while (next_key_value_str != NULL) {
        if (current_tuple_index == num_tuples_available) {
            log_printf(LOG_LEVEL_ERROR,
                       "Attempting to decode querystring with more key/value pairs than allocated Tuples!");
            return ESP_ERR_INVALID_SIZE;
        }

        char *equals_location = strchr(next_key_value_str, inner_delim);

        // Lengths do not include null terms. value_len subtracts an extra to account
        // the position of equals_location used in key_len calculation
        int key_len   = equals_location - next_key_value_str;
        int value_len = strlen(next_key_value_str) - key_len - 1;

        strncpy(tuple_array[current_tuple_index].key, next_key_value_str, key_len);
        strncpy(tuple_array[current_tuple_index].value, equals_location + 1, value_len + 1);

        // We already copy the null-term inclueded in next_key_value_str for the value, only need to add for key
        tuple_array[current_tuple_index].key[key_len] = '\0';
        // tuple_array[current_tuple_index].value[value_len] = '\0';

        current_tuple_index++;
        next_key_value_str = strtok(NULL, initial_delim);
    }

    return ESP_OK;
}
