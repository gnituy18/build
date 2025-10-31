#include <stdio.h>

void selection_sort(int[], int);

int main() {
  int arr[] = {5, 4, 3, 8, 11, 2, 1};
  int n = 7;

  selection_sort(arr, n);

  for (int i = 0; i < n; ++i) {
    printf("%d ", arr[i]);
  }

  return 0;
}

void selection_sort(int arr[], int n) {
  for (int i = 0; i < n - 1; ++i) {
    int minIdx = i;
    for (int j = i + 1; j < n; ++j) {
      if (arr[j] < arr[minIdx]) {
        minIdx = j;
      }
    }
    if (minIdx != i) {
      int tmp = arr[i];
      arr[i] = arr[minIdx];
      arr[minIdx] = tmp;
    }
  }

  return;
}
