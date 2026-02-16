#define A 0
#if A
int a=1;
#elif defined(A)
int a=2;
#else
int a=3;
#endif
