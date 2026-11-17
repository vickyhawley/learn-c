#include <stdio.h>

int is_even(int n);

int main(void) {
    
    unsigned char a = 4;
    unsigned char b = 7;
    is_even(a) == 1 ? printf("even") : printf("odd");
    is_even(b) == 1 ? printf("even") : printf("odd");
}

int is_even(int n){
    if(n % 2 == 0){ return 1;}
        else
        {return 0;}
}