typedef struct Listelem Listelem;

struct Listelem {
	void *front;
	void *back;
};

Listelem* listinit(Listelem*);
Listelem* listlink(Listelem*, Listelem*);
Listelem* listunlink(Listelem*);
int listisempty(Listelem*);
int listisfirst(Listelem*, Listelem*);
int listislast(Listelem*, Listelem*);

#define listeach(type, sentinel, ptr) \
	for(type _next = (sentinel)->front; \
	    (ptr) = _next, _next = (ptr)->front, (ptr) != (sentinel); )

#define listrange(type, sentinel, ptr) \
	for(type _next; \
	    _next = (ptr)->front, (ptr) != (sentinel); \
	    (ptr) = _next)

