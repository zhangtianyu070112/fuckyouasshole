#include <stdio.h>

int main() {
    const char* str = "\"bbox\": [0.010, 0.115, 0.083, 0.150]}";
    float x1=0, y1=0, x2=0, y2=0;
    int ret = sscanf(str, "\"bbox\": [%f, %f, %f, %f]", &x1, &y1, &x2, &y2);
    printf("ret=%d, y1=%f\n", ret, y1);
    return 0;
}