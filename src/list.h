enum { Listlead = 0xAA };

typedef struct List List;

/* Must be embedded at the top of struct */
struct List {
	uchar tag;
	List *link;
	List *tail;
};

/* What. */
#define foreach(type, list) \
	for(type ptr = listislead((list)) ? (type)(list)->link : (list); ptr->tag != Listlead; ptr = (type)ptr->link)

List* listalloc(void);
List* listlink(List*, List*);
List* listunlink(List*);
int listisempty(List*);
int listislead(List*);
int listisfirst(List*);
int listislast(List*);
