#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <iomanip>

// ============ 所有排序算法 ============

// 冒泡排序
void bubbleSort(std::vector<int>& arr) {
    int n = arr.size();
    for (int i = 0; i < n - 1; i++) {
        bool swapped = false;
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                std::swap(arr[j], arr[j + 1]);
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

// 选择排序
void selectionSort(std::vector<int>& arr) {
    int n = arr.size();
    for (int i = 0; i < n - 1; i++) {
        int minIdx = i;
        for (int j = i + 1; j < n; j++) {
            if (arr[j] < arr[minIdx]) minIdx = j;
        }
        std::swap(arr[i], arr[minIdx]);
    }
}

// 插入排序
void insertionSort(std::vector<int>& arr) {
    int n = arr.size();
    for (int i = 1; i < n; i++) {
        int key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// 快速排序
int partition(std::vector<int>& arr, int low, int high) {
    int pivot = arr[high];
    int i = low - 1;
    for (int j = low; j < high; j++) {
        if (arr[j] <= pivot) {
            i++;
            std::swap(arr[i], arr[j]);
        }
    }
    std::swap(arr[i + 1], arr[high]);
    return i + 1;
}

void quickSort(std::vector<int>& arr, int low, int high) {
    if (low < high) {
        int pi = partition(arr, low, high);
        quickSort(arr, low, pi - 1);
        quickSort(arr, pi + 1, high);
    }
}

void quickSortWrapper(std::vector<int>& arr) {
    quickSort(arr, 0, arr.size() - 1);
}

// 归并排序
void merge(std::vector<int>& arr, int left, int mid, int right) {
    int n1 = mid - left + 1;
    int n2 = right - mid;
    std::vector<int> L(n1), R(n2);
    for (int i = 0; i < n1; i++) L[i] = arr[left + i];
    for (int j = 0; j < n2; j++) R[j] = arr[mid + 1 + j];
    int i = 0, j = 0, k = left;
    while (i < n1 && j < n2) {
        if (L[i] <= R[j]) arr[k++] = L[i++];
        else arr[k++] = R[j++];
    }
    while (i < n1) arr[k++] = L[i++];
    while (j < n2) arr[k++] = R[j++];
}

void mergeSort(std::vector<int>& arr, int left, int right) {
    if (left < right) {
        int mid = left + (right - left) / 2;
        mergeSort(arr, left, mid);
        mergeSort(arr, mid + 1, right);
        merge(arr, left, mid, right);
    }
}

void mergeSortWrapper(std::vector<int>& arr) {
    mergeSort(arr, 0, arr.size() - 1);
}

// 堆排序
void heapify(std::vector<int>& arr, int n, int i) {
    int largest = i;
    int left = 2 * i + 1;
    int right = 2 * i + 2;
    if (left < n && arr[left] > arr[largest]) largest = left;
    if (right < n && arr[right] > arr[largest]) largest = right;
    if (largest != i) {
        std::swap(arr[i], arr[largest]);
        heapify(arr, n, largest);
    }
}

void heapSort(std::vector<int>& arr) {
    int n = arr.size();
    for (int i = n / 2 - 1; i >= 0; i--) heapify(arr, n, i);
    for (int i = n - 1; i > 0; i--) {
        std::swap(arr[0], arr[i]);
        heapify(arr, i, 0);
    }
}

// 计数排序（仅适用于非负整数）
void countingSort(std::vector<int>& arr) {
    if (arr.empty()) return;
    int maxVal = *std::max_element(arr.begin(), arr.end());
    std::vector<int> count(maxVal + 1, 0);
    for (int x : arr) count[x]++;
    int idx = 0;
    for (int i = 0; i <= maxVal; i++) {
        while (count[i] > 0) {
            arr[idx++] = i;
            count[i]--;
        }
    }
}

// 基数排序（基于计数排序，处理非负整数）
void countingSortForRadix(std::vector<int>& arr, int exp) {
    int n = arr.size();
    std::vector<int> output(n);
    std::vector<int> count(10, 0);
    for (int i = 0; i < n; i++) count[(arr[i] / exp) % 10]++;
    for (int i = 1; i < 10; i++) count[i] += count[i - 1];
    for (int i = n - 1; i >= 0; i--) {
        output[count[(arr[i] / exp) % 10] - 1] = arr[i];
        count[(arr[i] / exp) % 10]--;
    }
    for (int i = 0; i < n; i++) arr[i] = output[i];
}

void radixSort(std::vector<int>& arr) {
    if (arr.empty()) return;
    int maxVal = *std::max_element(arr.begin(), arr.end());
    for (int exp = 1; maxVal / exp > 0; exp *= 10) {
        countingSortForRadix(arr, exp);
    }
}

// ============ 工具函数 ============

bool isSorted(const std::vector<int>& arr) {
    for (size_t i = 1; i < arr.size(); i++) {
        if (arr[i - 1] > arr[i]) return false;
    }
    return true;
}

template <typename Func>
long long timeTest(std::vector<int>& data, Func func) {
    auto start = std::chrono::high_resolution_clock::now();
    func(data);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
}

// 生成各种数据分布
std::vector<int> genRandom(int n) {
    std::vector<int> v(n);
    for (int i = 0; i < n; i++) v[i] = std::rand() % 100000;
    return v;
}

std::vector<int> genSorted(int n) {
    std::vector<int> v(n);
    for (int i = 0; i < n; i++) v[i] = i;
    return v;
}

std::vector<int> genReverseSorted(int n) {
    std::vector<int> v(n);
    for (int i = 0; i < n; i++) v[i] = n - i;
    return v;
}

std::vector<int> genNearlySorted(int n) {
    std::vector<int> v(n);
    for (int i = 0; i < n; i++) v[i] = i;
    for (int i = 0; i < n / 20; i++) {
        int a = std::rand() % n;
        int b = std::rand() % n;
        std::swap(v[a], v[b]);
    }
    return v;
}

// 测试特定数据分布下所有算法
void testDistribution(const std::string& distName, const std::vector<int>& data) {
    std::cout << "\n========== " << distName << " (n=" << data.size() << ") ==========" << std::endl;
    std::cout << "--------------------------------------------" << std::endl;

    long long t;
    std::vector<int> tmp;

    tmp = data; t = timeTest(tmp, bubbleSort);
    std::cout << std::left << std::setw(12) << "Bubble" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, selectionSort);
    std::cout << std::left << std::setw(12) << "Selection" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, insertionSort);
    std::cout << std::left << std::setw(12) << "Insertion" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, quickSortWrapper);
    std::cout << std::left << std::setw(12) << "Quick" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, mergeSortWrapper);
    std::cout << std::left << std::setw(12) << "Merge" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, heapSort);
    std::cout << std::left << std::setw(12) << "Heap" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, countingSort);
    std::cout << std::left << std::setw(12) << "Counting" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, radixSort);
    std::cout << std::left << std::setw(12) << "Radix" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;

    tmp = data; t = timeTest(tmp, [](std::vector<int>& v){ std::sort(v.begin(), v.end()); });
    std::cout << std::left << std::setw(12) << "STL sort" << " : " << std::right << std::setw(10) << t
              << " us | 排序正确: " << (isSorted(tmp) ? "OK" : "FAIL") << std::endl;
}

// 测试不同规模下的性能（导出CSV数据）
void scalabilityTest() {
    std::cout << "\n========== 规模扩展测试 (随机数据) ==========" << std::endl;
    std::cout << "--------------------------------------------" << std::endl;

    std::ofstream csv("scalability.csv");
    csv << "size,bubble,selection,insertion,quick,merge,heap,counting,radix,stl\n";

    int sizes[] = {100, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000};

    for (int n : sizes) {
        std::vector<int> data = genRandom(n);
        std::cout << "n=" << n << ":" << std::endl;

        std::vector<int> bubble, selection, insertion, quick, merge, heap, counting, radix, stl;
        bubble = selection = insertion = quick = merge = heap = counting = radix = stl = data;

        long long bubble_t = timeTest(bubble, bubbleSort);
        long long selection_t = timeTest(selection, selectionSort);
        long long insertion_t = timeTest(insertion, insertionSort);
        long long quick_t = timeTest(quick, quickSortWrapper);
        long long merge_t = timeTest(merge, mergeSortWrapper);
        long long heap_t = timeTest(heap, heapSort);
        long long counting_t = timeTest(counting, countingSort);
        long long radix_t = timeTest(radix, radixSort);
        long long stl_t = timeTest(stl, [](std::vector<int>& v){ std::sort(v.begin(), v.end()); });

        csv << n << "," << bubble_t << "," << selection_t << "," << insertion_t << ","
            << quick_t << "," << merge_t << "," << heap_t << "," << counting_t << ","
            << radix_t << "," << stl_t << "\n";

        std::cout << "  Bubble=" << std::setw(8) << bubble_t
                  << " Selection=" << std::setw(8) << selection_t
                  << " Insertion=" << std::setw(8) << insertion_t
                  << " Quick=" << std::setw(8) << quick_t
                  << " Merge=" << std::setw(8) << merge_t
                  << " Heap=" << std::setw(8) << heap_t
                  << " Counting=" << std::setw(8) << counting_t
                  << " Radix=" << std::setw(8) << radix_t
                  << " STL=" << std::setw(8) << stl_t << " us" << std::endl;
    }
    csv.close();
    std::cout << "\nCSV 数据已保存到 scalability.csv" << std::endl;
}

int main() {
    std::srand(std::time(0));

    // 测试不同数据分布
    const int N = 10000;
    testDistribution("随机数据", genRandom(N));
    testDistribution("已排序数据", genSorted(N));
    testDistribution("逆序数据", genReverseSorted(N));
    testDistribution("近似有序", genNearlySorted(N));

    // 规模扩展测试
    scalabilityTest();

    return 0;
}
