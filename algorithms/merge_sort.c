#include <stdio.h>

void merge_sort(int[], int, int);
void merge(int[], int, int, int);

int main() {
  int arr[] = {5, 2, 9, 1, 5, 6};
  int n = 6;
  merge_sort(arr, 0, n - 1);

  for (int i = 0; i < n; ++i) {
    printf("%d ", arr[i]);
  }

  return 0;
}

void merge_sort(int arr[], int p, int r) {
  if (r - p == 0) {
    return;
  }

  int q = (p + r) / 2;
  merge_sort(arr, p, q);
  merge_sort(arr, q + 1, r);
  merge(arr, p, q, r);
}

void merge(int arr[], int p, int q, int r) {
  int nl = q - p + 1;
  int left[nl];
  for (int i = 0; i <= nl; ++i) {
    left[i] = arr[p + i];
  }

  int nr = r - q;
  int right[nr];
  for (int i = 0; i <= nr; ++i) {
    right[i] = arr[i + q + 1];
  }

  int i = 0;
  int j = 0;
  int k = p;

  while (i < nl && j < nr) {
    if (left[i] < right[j]) {
      arr[k++] = left[i++];
    } else {
      arr[k++] = right[j++];
    }
  }

  if (i < nl) {
    while (i < nl)
      arr[k++] = left[i++];
  }
  if (j < nr) {
    while (j < nr)
      arr[k++] = right[j++];
  }
}
