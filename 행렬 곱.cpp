#include <iostream>
#include <chrono>

int main() {
	using namespace std::chrono;

	unsigned int N = 1024;
	unsigned int* h_A = new unsigned int[N * N];
	unsigned int* h_B = new unsigned int[N * N];
	unsigned int* h_C = new unsigned int[N * N];
	for (unsigned int i = 0; i < N * N; ++i) {
		h_A[i] = i;
		h_B[i] = i;
		h_C[i] = 0;
	}

	auto start_t = high_resolution_clock::now();
	/*for (unsigned int y = 0; y < N; ++y)
		for (unsigned int x = 0; x < N; x++) 
			for (unsigned int i = 0; i < N; ++i)
				h_C[y * N + x] += h_A[i + y * N] * h_B[i * N + x];*/

	for (unsigned int y = 0; y < N; ++y)
		for (unsigned int x = 0; x < N; x++)
			for (unsigned int i = 0; i < N; ++i)
				h_C[y * N + i] += h_A[x + y * N] * h_B[x * N + i];
		
	auto end_t = high_resolution_clock::now();
	auto exec_t = duration_cast<milliseconds>(end_t - start_t).count();

	std::cout << "Execution Time : " << exec_t << " msec\n";		// 4600 ~ 4700 msec / 최적화 하면 400 ~ 500 msec

}