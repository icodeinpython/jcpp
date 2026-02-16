#define FLAG 1
#ifdef FLAG
int a=1;
#endif
#undef FLAG
#ifndef FLAG
int b=2;
#endif
