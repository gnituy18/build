#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void bubble_sort(int arr[], int size) {
  for (int i = 0; i < size - 1; ++i) {
    bool swapped = false;
    for (int j = 0; j < size - i - 1; ++j) {
      if (arr[j] > arr[j + 1]) {
        int temp = arr[j];
        arr[j] = arr[j + 1];
        arr[j + 1] = temp;
        swapped = true;
      }
    }
    if (!swapped) {
      break;
    }
  }
}

int main() {
  srand(time(NULL));
  int arr[10];
  for (int i = 0; i < 10; ++i) {
    arr[i] = rand() % 100;
  }

  bubble_sort(arr, 10);

  for (int i = 0; i < 10; ++i) {
    printf("%d\n", arr[i]);
  }
}
