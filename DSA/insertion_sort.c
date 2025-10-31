#include <stdio.h>

void insertion_sort(int[], int);

int main() {
  int arr[] = {5, 4, 3, 8, 11, 2, 1};
  int n = 7;

  insertion_sort(arr, n);

  for (int i = 0; i < n; ++i) {
    printf("%d ", arr[i]);
  }

  return 0;
}

void insertion_sort(int arr[], int n) {
  for (int i = 1; i < n; ++i) {
    int key = arr[i];
    int j = i - 1;
    while (key < arr[j] && j >= 0) {
      int tmp = arr[j+1];
      arr[j+1] = arr[j];
      arr[j] = tmp;
      --j;
    }

    arr[j+1] = key;
  }
}
