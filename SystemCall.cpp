#include <iostream>
#include <chrono>
#include <thread>
using namespace std;
using namespace chrono;
int main()
{
	volatile long long tmp = 0;
	auto start = high_resolution_clock::now();
	for (int j = 0; j <= 1000'0000; ++j) {
		tmp += j;
		this_thread::yield();		// 호출 안하면 2밀리sec / 호출하면 2063밀리sec (거의 1000배는 더 걸림 -> 운영체제 호출하면 안되는 이유)
	}
	auto duration = high_resolution_clock::now() - start;
	cout << "Time " << duration_cast<milliseconds>(duration).count();
	cout << " msec\n";
	cout << "RESULT " << tmp << endl;
}