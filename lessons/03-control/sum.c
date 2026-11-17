#include <stdio.h>

int sum_to(int n);

int main(void) {
    printf("%d\n", sum_to(10));
}

int sum_to(int n){
    int sum = 0;
    for(int i = 0; i <= n; i ++){
        sum += i;
    }
    return sum;
}

