#include <stdio.h>
int main(void) {
    printf("char: %zu bytes\n", sizeof(char));
    printf("int: %zu bytes\n", sizeof(int));
    printf("float: %zu bytes\n", sizeof(float));
    printf("short: %zu bytes\n", sizeof(short));
    printf("double: %zu bytes\n", sizeof(double));
    printf("long: %zu bytes\n", sizeof(long));
    unsigned char small = 255;
    small = small + 1;
    printf("%d\n", small);
    return 0;
}