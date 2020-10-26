#ifndef URL_DECODE_H
#define URL_DECODE_H

typedef struct {
    char key[30];
    char value[30];
} Tuple;

void urldecode2(char *dst, const char *src);
int get_key_values(char *decoded_str, Tuple *tuple_array, int num_tuples_available);

#endif
