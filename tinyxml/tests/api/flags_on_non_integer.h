// expect error: TXML_FLAGS needs an integer
enum TXML_FLAG_SET(txml_bits, "TXML_BIT_") { TXML_BIT_A = 1u };
typedef struct { int x; } txml_pair;
TXML_API txml_pair txml_pair_get(void) TXML_FLAGS(txml_bits);
