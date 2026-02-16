#define STR(x) #x
#define CAT(a,b) a ## b
int z=CAT(1,2);
const char *s=STR(hello);
