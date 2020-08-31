extern int DEBUG;

#define D(force, code) \
	if((DEBUG) || (force)) do{code}while(0);

void dprint(int force, char *fmt, ...);

void* emalloc(ulong sz);
char* estrdup(char *s);
