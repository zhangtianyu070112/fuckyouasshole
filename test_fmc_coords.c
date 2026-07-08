#include <stdio.h>
#include <string.h>

int main() {
    float lsk_y_centers[6] = {0.1325, 0.1825, 0.2295, 0.2775, 0.3255, 0.3755};
    int rect_h = 992;
    int sh = 0.350 * 992;
    int line_h = sh / 14;
    
    for (int ln = 0; ln < 12; ln++) {
        int lsk_idx = ln / 2;
        int is_data = ln % 2;
        int y = (int)(lsk_y_centers[lsk_idx] * rect_h);
        if (!is_data) {
            y -= line_h;
        }
        printf("Line %d (LSK %d %s): y = %d\n", ln, lsk_idx+1, is_data ? "DATA" : "HEAD", y);
    }
    return 0;
}