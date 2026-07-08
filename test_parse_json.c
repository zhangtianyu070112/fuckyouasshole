#include <stdio.h>
#include <string.h>

int main() {
    FILE* f = fopen("location-FMC.json", "r");
    if (!f) { printf("Failed to open\n"); return 1; }
    char line[512];
    int in_screen = 0;
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char* label_ptr = strstr(line, "\"label\"");
        char* bbox_ptr = strstr(line, "\"bbox\"");
        
        if (label_ptr && strstr(label_ptr, "DISPLAY_SCREEN")) {
            in_screen = 1;
        } else if (in_screen && bbox_ptr) {
            in_screen = 0;
        } else if (label_ptr && bbox_ptr) {
            char label[32] = {0};
            float x1=0, y1=0, x2=0, y2=0;
            char* val = strchr(label_ptr + 7, '"');
            if (val) {
                val++;
                char* end = strchr(val, '"');
                if (end) {
                    int len = end - val;
                    if (len > 31) len = 31;
                    strncpy(label, val, len);
                }
            }
            int ret = sscanf(bbox_ptr, "\"bbox\": [%f, %f, %f, %f]", &x1, &y1, &x2, &y2);
            if (strncmp(label, "LSK_", 4) == 0 && label[5] == 'L') {
                printf("%s: %f %f\n", label, y1, y2);
            }
            count++;
        }
    }
    printf("Total: %d\n", count);
    return 0;
}