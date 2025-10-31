#include <stdio.h>

#define MAX 100

struct heap {
  int len;
  int arr[MAX];
};

int main() {
  struct heap h;
  h.len = MAX;
  for (int i = 0; i < MAX; i++) {
    h.arr[i] = 0;
  }

  return 0;
}

void max_heapify(struct heap *h, int i) {
  int l = h->arr[i * 2 + 1];
  int r = h->arr[i * 2 + 2];
  int largest = i;
  if (l < h->len && h->arr[l] > h->arr[largest]) {
    largest = l;
  }
  if (r < h->len && h->arr[r] > h->arr[largest]) {
    largest = r;
  }

  if (largest != i) {
    int temp = h->arr[largest];
    h->arr[largest] = h->arr[i];
    h->arr[i] = temp;
    max_heapify(h, largest);
  }
}
