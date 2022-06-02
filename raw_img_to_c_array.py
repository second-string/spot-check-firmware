import re
import numpy as np


def bytes_to_c_arr(data):
    return [format(b, '#04x') for b in data]


def read_file(file_name):
    data = np.fromfile(file_name, dtype='uint8')
    data = bytearray(data)
    return data


def write_c_array(file_name, data):
    c_file = open(file_name + ".c", "w")
    static_content = "const unsigned char raw_image[" + str(len(data)) + "] = {\n"
    array_content = ", ".join(bytes_to_c_arr(data))
    array_content = re.sub("(.{72})", "    \\1\n", array_content, 0, re.DOTALL)
    final_content = static_content + array_content + "\n};"
    c_file.write(final_content)

raw_data = read_file("/Users/brianteam/Developer/spot-check-api/temp_renders/smol_render.raw")
write_c_array("./main/raw_image", raw_data)
