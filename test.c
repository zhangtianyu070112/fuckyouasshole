#include <stdio.h>
int main() { float x1, y1, x2, y2; int r = sscanf("\"bbox\": [0.010, 0.115, 0.083, 0.150]", "\"bbox\": [%f, %f, %f, %f]", &x1, &y1, &x2, &y2); printf("%d %f %f\n", r, x1, y1); return 0; }
