#include <stdio.h>
#include <string.h>
int main() {
    char line[] = "      \"bbox\": [0.160, 0.075, 0.840, 0.425]";
    char* bbox_ptr = strstr(line, "\"bbox\"");
    float x1=0, y1=0, x2=0, y2=0;
    int res = sscanf(bbox_ptr, "\"bbox\": [%f, %f, %f, %f]", &x1, &y1, &x2, &y2);
    printf("res=%d, x1=%f, y1=%f, x2=%f, y2=%f\n", res, x1, y1, x2, y2);
    return 0;
}