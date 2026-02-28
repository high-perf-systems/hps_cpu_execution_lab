#include <chrono>
#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <cstdlib>

using namespace std;
using clock_type = chrono::steady_clock;

static inline void escape(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

int main(int argc, char** argv)
{
    const size_t N = argc > 1 ? stoull(argv[1]) : 100000000;
    // fill with random values 0 - 255
    vector<uint8_t> data(N);
    for(size_t i=0;i<N;i++)
    {
        data[i] = rand() % 256;
    }

    int64_t sum = 0;

    //PREDICTABLE
    // the array is sorted , hence if data[i] >= 128 is used as condition
   // in the if condition, we will have a series of misses and then hits.
   // the compiler might predict the pattern and accordingly execute out of order 
   // execution
   sort(data.begin(), data.end());
   // warm-up
   for(size_t i=0;i<N;i++)
   {
     if (data[i]>=128) sum += data[i];
   }
   escape(&sum);

   sum = 0;
   auto start = clock_type::now();
    for(size_t i=0;i<N;i++)
   {
     if (data[i]>=128) sum += data[i];
   }
   auto end = clock_type::now();
   escape(&sum);

   auto t = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    std::cout << "Predictable (sorted) time : " << t.count() << " us\n";
    std::cout << "Sum                       : " << sum << "\n";
    return 0;
}