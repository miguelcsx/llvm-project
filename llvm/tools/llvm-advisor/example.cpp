#include <iostream>

int sum_of_squares(int n) {
    int sum = 0;

    for (int i = 1; i <= n; ++i) {
        int sq = i * i;

        if (sq % 2 == 0)
            sum += sq;
        else
            sum -= sq;
    }

    return sum;
}

int main() {
    int n = 5;
    std::cout << sum_of_squares(n) << std::endl;
}
