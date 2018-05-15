#define FUSE_USE_VERSION 26
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fuse.h>
#include <sys/mman.h>

//#define debugprint

#ifdef debugprint
#include <stdio.h>
#define DEBUGLOG printf("yes\n");
#endif // debugprint

#define min(a,b) ((a) < (b) ? (a) : (b))

#define fastmode		//在这个模式下，必须保证共用体文件夹和文件对应成员完全对齐，能加速

#define MAXSIZE (2 * 1024 * 1024 * (size_t)1024)     //2GB
#define BLOCKSIZE (4 * (size_t)1024)                 //4KB	注：不要修改！
#define MAXNAMELEN 256								//文件名最长255个字符
#define PATHMAXLEN 4096								//路径名最长4095个字符

static const size_t size = MAXSIZE;                         //size = 2gb
static const size_t blocksize = BLOCKSIZE;                  //blocksize = 4kb
static const size_t blocknr = MAXSIZE / BLOCKSIZE;          //blocknr = num of block
static int cachemode = 1;							//加速大文件写入 只在块大小为4k时有效


/**********************************数据结构定义****************************************/

typedef enum blockkind{efile, edir, edata} Blockkind;

/******这段因为对齐原因不能用，不过还是使用这种结构********/
/*
typedef struct filenode {
    char filename[MAXNAMELEN];
    struct stat filest;
    int next;
    int nextcontent;

#if test2 == 4096
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2 - 8];		//能写一部分东西
#elif test2 == 4092
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2 - 4];
#else
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2];
#endif	//test2

}Filenode;

typedef struct dirnode{
	char dirname[MAXNAMELEN];
	struct stat dirst;
	int next;
	int firstchild;

#if test2 == 4096
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2 - 8];
#elif test2 == 4092
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2 - 4];
#else
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(int) * 2];
#endif	//test2

}Dirnode;

typedef struct datanode{
	int nextdata;

#if test2 == 4096
	char datacontent[BLOCKSIZE - sizeof(Blockkind) - sizeof(int) - 8];
#elif test2 == 4092
	char datacontent[BLOCKSIZE - sizeof(Blockkind) - sizeof(int) - 4];
#else
	char datacontent[BLOCKSIZE - sizeof(Blockkind) - sizeof(int)];
#endif // test2

}Datanode;

typedef struct blocknode{
	Blockkind kind;
	int blocknum;
	union{
		Filenode file;
		Dirnode dir;
		Datanode data;
	};
}Blocknode;


Blocknode testsize;
static const size_t DATACONTENTSIZE = sizeof(testsize.data.content);	//constant
static const size_t CONTENTSIZE = sizeof(testsize.file.content);	//constant
*/

/******手动实现结构体******/

#define CKINDOFFSET 0
#define CKINDSIZE (sizeof(Blockkind))

#define CBLOCKNUMOFFSET CKINDSIZE
#define CBLOCKNUMSIZE (sizeof(int))

#define CFILENAMEOFFSET (CBLOCKNUMOFFSET + CBLOCKNUMSIZE)
#define CFILENAMESIZE MAXNAMELEN

#define CFILESTOFFSET (CFILENAMEOFFSET + CFILENAMESIZE)
#define CFILESTSIZE (sizeof(struct stat))

#define CNEXTOFFSET (CFILESTOFFSET + CFILESTSIZE)
#define CNEXTSIZE (sizeof(int))

#define CNEXTCONTENTOFFSET (CNEXTOFFSET + CNEXTSIZE)
#define CNEXTCONTENTSIZE (sizeof(int))

#define CCONTENTOFFSET (CNEXTCONTENTOFFSET + CNEXTCONTENTSIZE)
#define CCONTENTSIZE (BLOCKSIZE - CCONTENTOFFSET)

#define CDIRNAMEOFFSET CFILENAMEOFFSET
#define CDIRNAMESIZE CFILENAMESIZE

#define CDIRSTOFFSET CFILESTOFFSET
#define CDIRSTSIZE CFILESTSIZE

#define CFIRSTCHILDOFFSET CNEXTCONTENTOFFSET
#define CFIRSTCHILDSIZE CNEXTCONTENTSIZE

#define CNOTUSEOFFSET CCONTENTOFFSET
#define CNOTUSESIZE CCONTENTSIZE

#define CNEXTDATAOFFSET CFILENAMEOFFSET
#define CNEXTDATASIZE (sizeof(int))

#define CDATACONTENTOFFSET (CNEXTDATAOFFSET + CNEXTDATASIZE)
#define CDATACONTENTSIZE (BLOCKSIZE - CDATACONTENTOFFSET)

#ifdef fastmode
//这个快速模式所有用到的，节省空间
static int KINDOFFSET,BLOCKNUMOFFSET,FILENAMEOFFSET,DIRNAMEOFFSET,NEXTOFFSET,NEXTDATAOFFSET,NEXTCONTENTOFFSET;
static int FILESTOFFSET,FIRSTCHILDOFFSET,DIRSTOFFSET,DATACONTENTOFFSET,CONTENTSIZE,DATACONTENTSIZE,CONTENTOFFSET;

#else

static int KINDOFFSET, KINDSIZE ,BLOCKNUMOFFSET, BLOCKNUMSIZE, FILENAMEOFFSET, FILENAMESIZE, FILESTOFFSET, DATACONTENTSIZE;
static int FILESTSIZE, NEXTOFFSET ,NEXTSIZE, NEXTCONTENTOFFSET, NEXTCONTENTSIZE, CONTENTOFFSET, CONTENTSIZE, DIRNAMEOFFSET;
static int DIRNAMESIZE, DIRSTOFFSET ,DIRSTSIZE, FIRSTCHILDOFFSET, FIRSTCHILDSIZE, NEXTDATAOFFSET, NEXTDATASIZE, DATACONTENTOFFSET;

#endif // fastmode

//在init中算出这些常量提高速度

//REPLACE:
//Blocknode * -> char *
//x -> kind -> (*(Blockkind *)(x + KINDOFFSET))
//(x -> file).filename -> (x + FILENAMEOFFSET)
//(X -> file).filest -> (*(struct stat *)(x + FILESTOFFSET))
//(X -> file).next -> mem[*(int *)(x + NEXTOFFSET)]
//(X -> file).nextcontent -> mem[*(int *)(x + NEXTCONTENTOFFSET)]
//(X -> file).content -> (x + CONTENTOFFSET)



/********链队列,用于加速下一个空闲节点获取*********/
//用于加速下一个空闲结点的获取

typedef struct qnode{
    int data;
    struct qnode *next;
}QNode, *Qlink;

typedef struct linkqueue{
    Qlink head;
    Qlink tail;
    int length;
}LinkQueue;

static void initQueue(LinkQueue *q){
    q -> length = 0;
    q -> head = (Qlink)malloc(sizeof(QNode));  //头结点
    q -> tail = q -> head;
    q -> head -> next = NULL;
}

static void destroy(Qlink q){
	Qlink p;
    while(q){
		p = q -> next;
		free(q);
		q = p;
    }
}

static void destroyQueue(LinkQueue *q){
    destroy(q -> head);
}

static void enQueue(LinkQueue *q, int d){
    q -> tail = q -> tail -> next = (Qlink)malloc(sizeof(QNode));
    q -> length ++;
    q -> tail -> data = d;
    q -> tail -> next = NULL;
}

static int deQueue(LinkQueue *q, int *data){
    if(q -> head -> next == NULL)return 0;
    Qlink dele = q -> head -> next;
    q -> head -> next = q -> head -> next -> next;
    *data = dele -> data;
    free(dele);
    q -> length --;
	if(q -> head -> next == NULL)q -> tail = q -> head;
    return 1;
}

static LinkQueue Q;		//未用mem结点队列


/********类似cache，用于加速大文件写入********/

static char *last = NULL, *tail = NULL;		//last tail 上一次访问的文件、数据的尾部
static off_t lastoffset = 0, lastsize = 0;	//相当于一个cache用于提升write的速度。write总是4k4k的写
static char lastpath[PATHMAXLEN];					//上一次的path，用于cache



/************************************辅助函数、全局变量*************************************/

static char *mem[MAXSIZE / BLOCKSIZE];	//0用于存储头指针
static char *root = NULL;				//存放在mem[0]中

static char *mallocNode(Blockkind bk){
	int n;
	if(!deQueue(&Q, &n)) return NULL;
	char *p = (char *)mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mem[n] = p;
	//memset(p, 0, blocksize);  	//不需要他
	*(Blockkind *)(p + KINDOFFSET) = bk;
	*(int *)(p + BLOCKNUMOFFSET) = n;

#ifdef debugprint
	printf("malloc node pointer is %p \tuse mem[%d]\n", p, n);
#endif // debugprint

	return p;
}

static void freeNode(char *block){
	if(block) {
		int temp = *(int *)(block + BLOCKNUMOFFSET);
		if(!temp) return;
		mem[temp] = NULL;
		enQueue(&Q, temp);	//回收
		munmap(block, blocksize);
	}
}

static char *stringSplit(char **s, char c){  //找到字符串s中第一次出现的c，返回前一半，s指向后一半,长度小于PATHMAXLEN
    static char newstr[PATHMAXLEN];
    char *pos = strchr(*s,c);
    if(!pos){
		strcpy(newstr, *s);
		*s = NULL;
    }
    else{
		strncpy(newstr, *s, pos - *s);
		newstr[pos - *s] = '\0';
		*s = pos + 1;
	}
    return newstr;
}

#ifdef debugprint

static void traverseBlockInSameLayer(char *first){
//	printf("[debug] ");
//	char *t = first;
//	while(t){
//		printf("%s ", (t -> kind == efile) ? (t -> file).filename : (t -> dir).dirname);
//		t = (char *)((t -> kind == efile) ? (t -> file).next : (t -> dir).next);
//	}
//	printf("\n");
	return;
}

static void printNodeInfo(char *node){
	if(!node) {
		printf("[NodeInfo]\tnode is null\n");
		return;
	}
	switch((*(Blockkind *)(node + KINDOFFSET))){
		case efile: printf("[NodeInfo]\tnode kind is %s\n\t\tnode name is %s\n", "file", (node + FILENAMEOFFSET));break;
		case edir: printf("[NodeInfo]\tnode kind is %s\n\t\tnode name is %s\n", "dir", (node + DIRNAMEOFFSET));break;
		case edata: printf("[NodeInfo]\tnode kind is data\n");break;
	}
}

static void printFreeMem10(LinkQueue Q){
	Qlink p = Q.head;
	printf("[debug] free mem : ");
	int i = 0;
	while(p = p -> next){
		if(i ++ >= 10)break;
		printf("%d,", p -> data);
	}
	printf("\n");
}

#endif // debugprint

static char *getBlockNode(const char *name, int stopAtParent, char **filename)
{
	char *path = (char *)((name && *name == '/') ? name + 1 : name), *str = NULL;					//跳过开头的 /
    char *p = root, *q = NULL;
    while(path && *path && (p || stopAtParent)){
		str = stringSplit(&path, '/');
		if(!path || !*path){		//str表示文件
			if(stopAtParent){
				*filename = str;
				return q;
			}
#ifdef fastmode
			int f = 1;
			while(p && (f = strcmp((p + DIRNAMEOFFSET), str)))
				p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
			if(!f) return p;
#else
			while(p){
				int f = 1;
				while(p && (*(Blockkind *)(p + KINDOFFSET)) == edir && (f = strcmp((p + DIRNAMEOFFSET), str)))
					p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
				while(p && (*(Blockkind *)(p + KINDOFFSET)) == efile && (f = strcmp((p + FILENAMEOFFSET), str)))
					p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
				if(!f) return p;	//找到
			}
#endif // fastmode
			return NULL;	//没找到文件
		}
		else{		//进入文件夹
#ifdef fastmode
			again: while(p && strcmp((p + DIRNAMEOFFSET), str))
				p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找到同目录下一个文件夹
			if(!p) return NULL;
			else {
				if(*(Blockkind *)(p + KINDOFFSET) == efile) {
					p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
					goto again;
				}
				else {
					q = p;
					p = *(int *)(p + FIRSTCHILDOFFSET) ? mem[*(int *)(p + FIRSTCHILDOFFSET)] : NULL;
					goto finddir;
				}
			}

#else
			while(p){
				while(p && *(Blockkind *)(p + KINDOFFSET) == efile)
					p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找到同目录下一个文件夹
				while(p && *(Blockkind *)(p + KINDOFFSET) == edir && strcmp((p + DIRNAMEOFFSET), str))
					p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
				if(p && *(Blockkind *)(p + KINDOFFSET) == edir && !strcmp((p + DIRNAMEOFFSET), str)) {	//找到了
					q = p;
					p = *(int *)(p + FIRSTCHILDOFFSET) ? mem[*(int *)(p + FIRSTCHILDOFFSET)] : NULL;
					goto finddir;
				}
			}
#endif // fastmode
			return NULL;	//没找到文件夹
		}
		finddir: ;		//找到了文件夹，继续
    }
    return NULL;
}


static int createBlockNode(const char *path, const struct stat *st, Blockkind kind)		//先文件夹再文件 按字母顺序存储
{
    char *p, *q, *r;
    char *str = NULL;
	r = q = p = getBlockNode(path, 1, &str);
    if(r){	//不再根目录
		p = *(int *)(p + FIRSTCHILDOFFSET) ? mem[*(int *)(p + FIRSTCHILDOFFSET)] : NULL;
		int f = 1;
		while(p && (*(Blockkind *)(p + KINDOFFSET)) == edir && (f = strcmp((p + DIRNAMEOFFSET), str))){
			if(kind == edir && f > 0) break;
			q = p;		//记录前一个
			p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
		}
		while(p && (*(Blockkind *)(p + KINDOFFSET)) == efile && (f = strcmp((p + FILENAMEOFFSET), str))){
			if(kind == edir) break;		//创建文件夹 应该放在文件前面
			else if(f > 0) break;
			q = p;
			p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
		}
		if(!f) return 1;	//文件已存在 1
		if(kind == edir){		//找有没有重名文件
			char *s = p;
#ifdef fastmode
			while(s && (f = strcmp((s + DIRNAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;	//找下一个
			}
#else
			while(s && *(Blockkind *)(*(Blockkind *)(s + KINDOFFSET)) == edir && (f = strcmp((s + DIRNAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;	//找下一个
			}
			while(s && *(Blockkind *)(*(Blockkind *)(s + KINDOFFSET)) == efile && (f = strcmp((s + FILENAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;
			}
#endif // fastmode
			if(!f) return 1;	//文件已存在 1
		}

		//创建节点
		if(strlen(str) >= MAXNAMELEN - 1) return 4;		//name too long
		char *n = mallocNode(kind);
		if(!n) return 3;	//文件系统满了 3
#ifdef fastmode
		strcpy((n + FILENAMEOFFSET), str);
		*(struct stat *)(n + FILESTOFFSET) = *st;
		*(int *)(n + NEXTCONTENTOFFSET) = 0;
		*(int *)(n + NEXTOFFSET) = 0;
#else
		if(kind == efile){
			strcpy((n + FILENAMEOFFSET), str);
			*(struct stat *)(n + FILESTOFFSET) = *st;
			*(int *)(n + NEXTCONTENTOFFSET) = 0;
			*(int *)(n + NEXTOFFSET) = 0;
		}
		else {
			strcpy((n + DIRNAMEOFFSET), str);
			*(struct stat *)(n + DIRSTOFFSET) = *st;
			*(int *)(n + FIRSTCHILDOFFSET) = 0;
			*(int *)(n + NEXTOFFSET) = 0;
		}
#endif // fastmode
		//插入顺序链表
		if(r == q){		//r是空文件夹或应该存在第一个位置
			int *tp = (int *)(r + FIRSTCHILDOFFSET);
			int temp = *tp;
			*tp = *(int *)(n + BLOCKNUMOFFSET);
			*(int *)(n + NEXTOFFSET) = temp;
		}
		else{
			int *tp = (int *)(q + NEXTOFFSET);
			int temp = *tp;
			*tp = *(int *)(n + BLOCKNUMOFFSET);
			*(int *)(n + NEXTOFFSET) = temp;
		}
	}
	else{		//在根目录
		if(*path != '/')return 2;		//目录不存在 2
		p = root, q = NULL;
		int f = 1;
		while(p && (*(Blockkind *)(p + KINDOFFSET)) == edir && (f = strcmp((p + DIRNAMEOFFSET), str))){
			if(kind == edir && f > 0) break;
			q = p;		//记录前一个
			p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
		}
		while(p && (*(Blockkind *)(p + KINDOFFSET)) == efile && (f = strcmp((p + FILENAMEOFFSET), str))){
			if(kind == edir) break;
			else if(f > 0)break;
			q = p;
			p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
		}
		if(!f) return 1;	//文件已存在 1
		if(kind == edir){		//找有没有重名文件
			char *s = p;
#ifdef fastmode
			while(s && (f = strcmp((s + DIRNAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;	//找下一个
			}
#else
			while(s && (*(Blockkind *)(s + KINDOFFSET)) == edir && (f = strcmp((s + DIRNAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;	//找下一个
			}
			while(s && (*(Blockkind *)(s + KINDOFFSET)) == efile && (f = strcmp((s + FILENAMEOFFSET), str))){
				s = *(int *)(s + NEXTOFFSET) ? mem[*(int *)(s + NEXTOFFSET)] : NULL;
			}
#endif // fastmode
			if(!f) return 1;	//文件已存在 1
		}

		//创建节点
		if(strlen(str) >= MAXNAMELEN - 1) return 4;		//name too long
		char *n = mallocNode(kind);
		if(!n) return 3;	//文件系统满了 3
#ifdef fastmode
		strcpy((n + FILENAMEOFFSET), str);
		*(struct stat *)(n + FILESTOFFSET) = *st;
		*(int *)(n + NEXTCONTENTOFFSET) = 0;
		*(int *)(n + NEXTOFFSET) = 0;
#else
		if(kind == efile){
			strcpy((n + FILENAMEOFFSET), str);
			*(struct stat *)(n + FILESTOFFSET) = *st;
			*(int *)(n + NEXTCONTENTOFFSET) = 0;
			*(int *)(n + NEXTOFFSET) = 0;
		}
		else {
			strcpy((n + DIRNAMEOFFSET), str);
			*(struct stat *)(n + DIRSTOFFSET) = *st;
			*(int *)(n + FIRSTCHILDOFFSET) = 0;
			*(int *)(n + NEXTOFFSET) = 0;
		}
#endif // fastmode
		//插入顺序链表
		if(!q){		//要插在root
			char *temp = root;
			*(char **)(mem[0]) = root = n;
			if(temp){
				*(int *)(n + NEXTOFFSET) = *(int *)(temp + BLOCKNUMOFFSET);
			}
			else {
				*(int *)(n + NEXTOFFSET) = 0;
			}
		}
		else{
			int *tp = (int *)(q + NEXTOFFSET);
			int temp = *tp;
			*tp = *(int *)(n + BLOCKNUMOFFSET);
			*(int *)(n + NEXTOFFSET) = temp;
		}
	}

#ifdef debugprint
	traverseBlockInSameLayer(root);
#endif // debugprint

	return 0;	//0成功
}

static int deleteBlockNode(const char *path){
	char *str;
	int f = 0;
	char *r = getBlockNode(path, 1, &str), *q = NULL, *p = NULL;
	if(!r) p = root;
	else p = *(int *)(r + FIRSTCHILDOFFSET) ? mem[*(int *)(r + FIRSTCHILDOFFSET)] : NULL;
#ifdef fastmode
	while(p && (f = strcmp((p + DIRNAMEOFFSET), str))) {
		q = p;
		p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
	}
#else
	while(p && (*(Blockkind *)(p + KINDOFFSET)) == edir && (f = strcmp((p + DIRNAMEOFFSET), str))) {
		q = p;
		p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;	//找下一个
	}
	while(p && (*(Blockkind *)(p + KINDOFFSET)) == efile && (f = strcmp((p + FILENAMEOFFSET), str))) {
		q = p;
		p = *(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL;
	}
#endif // fastmode
	if(!p){
		return 1;	//file or dir not found 1
	}
	else if(!q){		//the first should be deleted
		if(p == root) {
			*(char **)(mem[0]) = root = (*(int *)(p + NEXTOFFSET) ? mem[*(int *)(p + NEXTOFFSET)] : NULL);
			freeNode(p);
		}
		else {
			*(int *)(r + FIRSTCHILDOFFSET) = *(int *)(p + NEXTOFFSET);
			freeNode(p);
		}
	}
	else {
		*(int *)(q + NEXTOFFSET) = *(int *)(p + NEXTOFFSET);
		freeNode(p);
	}
	return 0;	//succeed
}

static void destroyBlockDataLinkList(char *node){
	char *p = node, *q = node;
	while(p){
		q = *(int *)(p + NEXTDATAOFFSET) ? mem[*(int *)(p + NEXTDATAOFFSET)] : NULL;
		freeNode(p);
		p = q;
	}
}

static size_t getCurrentFileContentSize(char *node){
	char *p = node;
	size_t origsize = 0;
	while(p){
		if(*(Blockkind *)(p + KINDOFFSET) == efile) {
			origsize += CONTENTSIZE;
			p = *(int *)(p + NEXTCONTENTOFFSET) ? mem[*(int *)(p + NEXTCONTENTOFFSET)] : NULL;
		}
		else if(((*(Blockkind *)(p + KINDOFFSET)) == edata)) {
			origsize += DATACONTENTSIZE;
			p = *(int *)(p + NEXTDATAOFFSET) ? mem[*(int *)(p + NEXTDATAOFFSET)] : NULL;
		}
	}
	return origsize;
}

static int modifyCurrentFileContentSize(char *node, size_t targetSize){
	char *p = node, *q = NULL;
	size_t origsize = 0;
	while(p){
		if(*(Blockkind *)(p + KINDOFFSET) == efile) {
			origsize += CONTENTSIZE;
			if(origsize >= targetSize){
				int *ti = (int *)(p + NEXTCONTENTOFFSET);
				if(*ti) {
					destroyBlockDataLinkList(mem[*ti]);
					*ti = 0;
				}
			}
			q = p;
			p = *(int *)(p + NEXTCONTENTOFFSET) ? mem[*(int *)(p + NEXTCONTENTOFFSET)] : NULL;
		}
		else if(((*(Blockkind *)(p + KINDOFFSET)) == edata)) {
			origsize += DATACONTENTSIZE;
			if(origsize >= targetSize){
				int *ti = (int *)(p + NEXTDATAOFFSET);
				if(*ti) {
					destroyBlockDataLinkList(mem[*ti]);
					*ti = 0;
				}
			}
			q = p;
			p = *(int *)(p + NEXTDATAOFFSET) ? mem[*(int *)(p + NEXTDATAOFFSET)] : NULL;
		}
	}
	tail = q;
	off_t need = targetSize - origsize;
	lastsize = -need;
	if(need <= 0) return 0;
	while(origsize < targetSize){
		if(*(Blockkind *)(q + KINDOFFSET) == efile){
			char *t = mallocNode(edata);
			if(!t) return 1;
			q = mem[*(int *)(q + NEXTCONTENTOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
		}
		else if((*(Blockkind *)(q + KINDOFFSET)) == edata){
			char *t = mallocNode(edata);
			if(!t) return 1;
			q = mem[*(int *)(q + NEXTDATAOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
		}
		*(int *)(q + NEXTDATAOFFSET) = 0;
		origsize += DATACONTENTSIZE;
	}
	lastsize = origsize - targetSize;	//还差多少满
	tail = q;
	return 0;
}

static int adjustFileContent(char *node, off_t offset, const char *buf, size_t size){	//在node文件中offset处写入大小为size的buf
	int hit = 0;
	char *save;
	size_t hitoffset;
	if(cachemode && last == node && size == 4096 && offset - lastoffset == 4096)	//cache
		{	//hit

#ifdef debugprint
			printf("       hit! name is %s\n", (node + FILENAMEOFFSET));
#endif // debugprint

			hit = 1;
			save = tail;
			Blockkind k = *(Blockkind *)(tail + KINDOFFSET);
			hitoffset = (k == efile ? CONTENTSIZE : DATACONTENTSIZE) - lastsize;
			if(lastsize + DATACONTENTSIZE > 4096){	//need one block
				if(k == efile){
					char *t = mallocNode(edata);
					if(!t) return 1;
					tail = mem[*(int *)(tail + NEXTCONTENTOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
				}
				else {
					char *t = mallocNode(edata);
					if(!t) return 1;
					tail = mem[*(int *)(tail + NEXTDATAOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
				}
				*(int *)(tail + NEXTDATAOFFSET) = 0;
				lastsize += DATACONTENTSIZE - 4096;
			}
			else {	//need two blocks
				if(k == efile){
					char *t = mallocNode(edata);
					if(!t) return 1;
					tail = mem[*(int *)(tail + NEXTCONTENTOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
				}
				else {
					char *t = mallocNode(edata);
					if(!t) return 1;
					tail = mem[*(int *)(tail + NEXTDATAOFFSET) = *(int *)(t + BLOCKNUMOFFSET)];
				}
				char *tt = mallocNode(edata);
				if(!tt) return 1;
				tail = mem[*(int *)(tail + NEXTDATAOFFSET) = *(int *)(tt + BLOCKNUMOFFSET)];
				*(int *)(tail + NEXTDATAOFFSET) = 0;
				lastsize += DATACONTENTSIZE * 2 - 4096;
			}
		}

	else if(modifyCurrentFileContentSize(node, ((struct stat *)(node + FILESTOFFSET)) -> st_size)) return 1;	//空间不足
	lastoffset = offset;
	char *p = last = node;
	char *pos = (char *)buf;
	size_t needsize = size;
	off_t needoffset = offset;
	if(hit){
		p = save;
		needoffset = hitoffset;
	}
	while(p){
		if(*(Blockkind *)(p + KINDOFFSET) == efile) {
			if(CONTENTSIZE > needoffset){
				size_t ss = min(CONTENTSIZE - needoffset, needsize);
				memcpy((p + CONTENTOFFSET) + needoffset, pos, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= CONTENTSIZE;
			p = *(int *)(p + NEXTCONTENTOFFSET) ? mem[*(int *)(p + NEXTCONTENTOFFSET)] : NULL;
		}
		else {
			if(DATACONTENTSIZE > needoffset){
				size_t ss = min(DATACONTENTSIZE - needoffset, needsize);
				memcpy((p + DATACONTENTOFFSET) + needoffset, pos, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= DATACONTENTSIZE;
			p = *(int *)(p + NEXTDATAOFFSET) ? mem[*(int *)(p + NEXTDATAOFFSET)] : NULL;
		}
	}
	return 0;
}

static void readFileContent(char *node, char *buf, off_t offset, int size){
	char *p = node;
	char *pos = buf;
	int needsize = size;
	off_t needoffset = offset;
	while(p){
		if(*(Blockkind *)(p + KINDOFFSET) == efile) {
			if(CONTENTSIZE > needoffset){
				size_t ss = min(CONTENTSIZE - needoffset, needsize);
				memcpy(pos, (p + CONTENTOFFSET) + needoffset, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= CONTENTSIZE;
			p = *(int *)(p + NEXTCONTENTOFFSET) ? mem[*(int *)(p + NEXTCONTENTOFFSET)] : NULL;
		}
		else if(*(Blockkind *)(p + KINDOFFSET) == edata) {
			if(DATACONTENTSIZE > needoffset){
				size_t ss = min(DATACONTENTSIZE - needoffset, needsize);
				memcpy(pos, (p + DATACONTENTOFFSET) + needoffset, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= DATACONTENTSIZE;
			p = *(int *)(p + NEXTDATAOFFSET) ? mem[*(int *)(p + NEXTDATAOFFSET)] : NULL;
		}
	}
}


void destroyBlockNodeAndChildren(char *node){	//clear dir
	if(!node)return;
	if(*(Blockkind *)(node + KINDOFFSET) == efile) {
		int ti = *(int *)(node + NEXTOFFSET), tj = *(int *)(node + NEXTCONTENTOFFSET);
		if(ti) destroyBlockNodeAndChildren(mem[ti]);
		if(tj) destroyBlockDataLinkList(mem[tj]);	//destroy content
		freeNode(node);	//destroy filenode
	}
	else {
		int ti = *(int *)(node + NEXTOFFSET), tj = *(int *)(node + FIRSTCHILDOFFSET);
		if(ti) destroyBlockNodeAndChildren(mem[ti]);
		if(tj) destroyBlockNodeAndChildren(mem[tj]);
		freeNode(node);	//destroy dirnode
	}
}

static void renameBlock(char *node, const char *newname){
#ifdef fastmode
	strcpy((node + FILENAMEOFFSET), newname);
#else
	if(*(Blockkind *)(node + KINDOFFSET) == efile){
		strcpy((node + FILENAMEOFFSET), newname);
	}
	else {
		strcpy((node + DIRNAMEOFFSET), newname);
	}
#endif // fastmode
}

/************************************接口实现*************************************/


static void *oshfs_init(struct fuse_conn_info *conn)
{

	//计算各个常量
#ifdef fastmode
	//这个快速模式只计算用到的
	KINDOFFSET = CKINDOFFSET;
	BLOCKNUMOFFSET = CBLOCKNUMOFFSET;
	FILENAMEOFFSET = CFILENAMEOFFSET;
	DIRNAMEOFFSET = CDIRNAMEOFFSET;
	NEXTOFFSET = CNEXTOFFSET;
	NEXTDATAOFFSET = CNEXTDATAOFFSET;
	NEXTCONTENTOFFSET = CNEXTCONTENTOFFSET;
	FILESTOFFSET = CFILESTOFFSET;
	FIRSTCHILDOFFSET = CFIRSTCHILDOFFSET;
	DIRSTOFFSET = CDIRSTOFFSET;
	DATACONTENTOFFSET = CDATACONTENTOFFSET;
	CONTENTSIZE = CCONTENTSIZE;
	DATACONTENTSIZE = CDATACONTENTSIZE;
	CONTENTOFFSET = CCONTENTOFFSET;
#else
	KINDOFFSET = CKINDOFFSET;
	KINDSIZE = CKINDSIZE;
	BLOCKNUMOFFSET = CBLOCKNUMOFFSET;
	BLOCKNUMSIZE = CBLOCKNUMSIZE;
	FILENAMEOFFSET = CFILENAMEOFFSET;
	FILENAMESIZE = CFILENAMESIZE;
	FILESTOFFSET = CFILESTOFFSET;
	DATACONTENTSIZE = CDATACONTENTSIZE;
	FILESTSIZE = CFILESTSIZE;
	NEXTOFFSET = CNEXTOFFSET;
	NEXTSIZE = CNEXTSIZE;
	NEXTCONTENTOFFSET = CNEXTCONTENTOFFSET;
	NEXTCONTENTSIZE = CNEXTCONTENTSIZE;
	CONTENTOFFSET = CCONTENTOFFSET;
	CONTENTSIZE = CCONTENTSIZE;
	DIRNAMEOFFSET = CDIRNAMEOFFSET;
	DIRNAMESIZE = CDIRNAMESIZE;
	DIRSTOFFSET = CDIRSTOFFSET;
	DIRSTSIZE = CDIRSTSIZE;
	FIRSTCHILDOFFSET = CFIRSTCHILDOFFSET;
	FIRSTCHILDSIZE = CFIRSTCHILDSIZE;
	NEXTDATAOFFSET = CNEXTDATAOFFSET;
	NEXTDATASIZE = CNEXTDATASIZE;
	DATACONTENTOFFSET = CDATACONTENTOFFSET;
#endif // fastmode

	//初始化未用队列
#ifdef fastmode
	//直接初始化节省时间
    Qlink p = Q.head = (Qlink)malloc(sizeof(QNode));  //头结点
	int i;
	for(i = 1; i < blocknr; i ++){	//0用于存储头指针
		p = p -> next = (Qlink)malloc(sizeof(QNode));
		p -> data = i;
	}
	Q.tail = p;
	p -> next = NULL;
	Q.length = blocknr - 1;
#else
	initQueue(&Q);
	int i;
	for(i = 1; i < blocknr; i ++){	//0用于存储头指针
		enQueue(&Q, i);
	}
#endif // fastmode
	//初始化mem0 前n个字节存入头指针
	char *pp = (char *)mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mem[0] = pp;
	memset(pp, 0, sizeof(char *));	//root指针一开始是NULL 只用前n位存

	//检测是否可以使用cache
	if(BLOCKSIZE != 4096) cachemode = 0;		//不是4k的块不能加速大文件写入
	else cachemode = 1;

//    // Demo 1
//    for(int i = 0; i < blocknr; i++) {
//        mem[i] = mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
//        memset(mem[i], 0, blocksize);
//    }
//    for(int i = 0; i < blocknr; i++) {
//        munmap(mem[i], blocksize);
//    }
//    // Demo 2
//    mem[0] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
//    for(int i = 0; i < blocknr; i++) {
//        mem[i] = (char *)mem[0] + blocksize * i;
//        memset(mem[i], 0, blocksize);
//    }
//    for(int i = 0; i < blocknr; i++) {
//        munmap(mem[i], blocksize);
//    }

#ifdef debugprint
	printFreeMem10(Q);
#endif // debugprint

	return NULL;
}

static int oshfs_getattr(const char *path, struct stat *stbuf)
{

#ifdef debugprint
printf("[debug] getattr\n");
#endif // debugprint

    int ret = 0;
    char *node = getBlockNode(path, 0, NULL);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
#ifdef fastmode
		memcpy(stbuf, node + FILESTOFFSET, sizeof(struct stat));
#else
        memcpy(stbuf, (*(Blockkind *)(node + KINDOFFSET) == efile) ? (struct stat *)(node + FILESTOFFSET) : (struct stat *)(node + DIRSTOFFSET), sizeof(struct stat));
#endif // fastmode
    } else {
        ret = -ENOENT;
    }
    return ret;
}

static int oshfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

#ifdef debugprint
printf("[debug] readdir\n");
#endif // debugprint

	char *node = root;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    if(strcmp(path, "/")){
		node = getBlockNode(path, 0, NULL);
		if(!node) return -ENOENT;
		node = *(int *)(node + FIRSTCHILDOFFSET) ? mem[*(int *)(node + FIRSTCHILDOFFSET)] : NULL;
    }
    while(node) {
#ifdef fastmode
		filler(buf, node + FILENAMEOFFSET, (const struct stat *)(node + FILESTOFFSET), 0);
		node = *(int *)(node + NEXTOFFSET) ? mem[*(int *)(node + NEXTOFFSET)] : NULL;
#else
		if((*(Blockkind *)(node + KINDOFFSET)) == efile){
			filler(buf, (node + FILENAMEOFFSET), (node + FILESTOFFSET), 0);
			node = *(int *)(node + NEXTOFFSET) ? mem[*(int *)(node + NEXTOFFSET)] : NULL;
		}
		else {
			filler(buf, (node + DIRNAMEOFFSET),(node + DIRSTOFFSET), 0);
			node = *(int *)(node + NEXTOFFSET) ? mem[*(int *)(node + NEXTOFFSET)] : NULL;
		}
#endif // fastmode
    }
    return 0;
}

static int oshfs_mknod(const char *path, mode_t mode, dev_t dev)	//创建文件节点
{

#ifdef debugprint
printf("[debug] mknod : %s\n", path);
#endif // debugprint

	last = NULL;
    struct stat st;
    st.st_mode = S_IFREG | mode;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    switch(createBlockNode(path, &st, efile)){
		case 0: return 0;
		case 1: return -EEXIST; 	//文件已存在
		case 2: return -ENOENT;	//没有这个目录
		case 3: return -ENOMEM;	//内存满了
		case 4: return -ENAMETOOLONG;	//文件名太长
    }
}

static int oshfs_open(const char *path, struct fuse_file_info *fi)
{

#ifdef debugprint
printf("[debug] open\n");
#endif // debugprint

	char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(*(Blockkind *)(node + KINDOFFSET) != efile) return -EISDIR;		//是个文件夹
    return 0;
}


static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{			//他一次只调用这个写4k，效率极低

#ifdef debugprint
	printf("[debug] write : path = %s, size = %ld, offset = %ld\n", path, size, offset);
    //if(root) printf("        before write: (*(Blockkind *)(root + KINDOFFSET)) == %s!\n", (*(Blockkind *)(root + KINDOFFSET)) == efile ? "efile" : "edir");
#endif // debugprint

	char *node;
	if(cachemode && !strcmp(path, lastpath) && last) node = last;
    else {
		node = getBlockNode(path, 0, NULL);
		if(!node) return -ENOENT;		//没有找到文件
		if((*(Blockkind *)(node + KINDOFFSET)) != efile)return -EISDIR;		//是个文件夹
		strcpy(lastpath, path);
	}
	struct stat *sp = (struct stat *)(node + FILESTOFFSET);
    if(sp -> st_size < offset + size) sp -> st_size = offset + size;
    if(adjustFileContent(node, offset, buf, size)){
		last = NULL;
		return -ENOMEM;
	}

#ifdef debugprint
    //if(root) printf("        after write: (*(Blockkind *)(root + KINDOFFSET)) == %s!\n", (*(Blockkind *)(root + KINDOFFSET)) == efile ? "efile" : "edir");
#endif // debugprint

    return size;
}

static int oshfs_truncate(const char *path, off_t size)
{

#ifdef debugprint
printf("[debug] truncate : path = %s , size = %ld\n", path, size);
#endif // debugprint

	last = NULL;
    char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
	if(*(Blockkind *)(node + KINDOFFSET) != efile) return -EISDIR;		//是个文件夹
    ((struct stat *)(node + FILESTOFFSET)) -> st_size = size;
    if(modifyCurrentFileContentSize(node, size)) return -ENOMEM;	//空间不足
    return 0;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

#ifdef debugprint
printf("[debug] read\n");
#endif // debugprint

	last = NULL;
    char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(*(Blockkind *)(node + KINDOFFSET) != efile) return -EISDIR;		//是个文件夹
    int ret = size;
    if(offset + size > (*(struct stat *)(node + FILESTOFFSET)).st_size)
        ret = (*(struct stat *)(node + FILESTOFFSET)).st_size - offset;
	readFileContent(node, buf, offset, ret);
    return ret;
}

static int oshfs_unlink(const char *path)
{

#ifdef debugprint
printf("[debug] unlink : %s\n", path);
#endif // debugprint

	last = NULL;
	char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(*(Blockkind *)(node + KINDOFFSET) == edir){
		int ti = *(int *)(node + FIRSTCHILDOFFSET);
		if(ti) destroyBlockNodeAndChildren(mem[ti]);	//clear dir's children
		deleteBlockNode(path);
	}
	else{
		int ti = *(int *)(node + NEXTCONTENTOFFSET);
		if(ti) destroyBlockDataLinkList(mem[ti]);	//clear content
		deleteBlockNode(path);
	}
    return 0;
}

static int oshfs_mkdir(const char *path, mode_t mode)
{

#ifdef debugprint
printf("[debug] mkdir\n");
#endif // debugprint

	struct stat st;
    st.st_mode = S_IFDIR | mode;
    st.st_uid = fuse_get_context()->uid;
    st.st_gid = fuse_get_context()->gid;
    st.st_nlink = 1;
    st.st_size = 0;
    switch(createBlockNode(path, &st, edir)){
		case 0: return 0;
		case 1: return -EEXIST; 	//文件已存在
		case 2: return -ENOENT;	//没有这个目录
		case 3: return -ENOMEM;	//内存满了
		case 4: return -ENAMETOOLONG;	//文件名太长
    }
}

static int oshfs_rmdir(const char *path)
{

#ifdef debugprint
printf("[debug] rmdir\n");
#endif // debugprint

	char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(*(Blockkind *)(node + KINDOFFSET) == edir){
		if(*(int *)(node + FIRSTCHILDOFFSET) ? mem[*(int *)(node + FIRSTCHILDOFFSET)] : NULL) return -ENOTEMPTY;
		deleteBlockNode(path);
	}
	else{
		return -ENOTDIR;
	}
    return 0;
}

static int oshfs_opendir(const char *path, struct fuse_file_info *fi)
{

#ifdef debugprint
printf("[debug] opendir : %s\n", path);
#endif // debugprint

	if(!strcmp(path, "/")) return 0;
	char *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
    if(*(Blockkind *)(node + KINDOFFSET) == efile) return -ENOTDIR;		//是个文件
    return 0;
}

static int oshfs_rename(const char *path, const char *newname)
{

#ifdef debugprint
printf("[debug] rename : %s -> %s\n", path, newname);
#endif // debugprint

	char *node = getBlockNode(path, 0, NULL), *p;
	char *str, *q = (char *)newname;
	while(q) str = stringSplit(&q, '/');
	if(strlen(str) >= MAXNAMELEN - 1) return -ENAMETOOLONG;
	if(!node) return -ENOENT;		//没有找到文件
    renameBlock(node, str);

#ifdef debugprint
    printf("        new name is : %s\n", str);
#endif // debugprint

    return 0;
}

static int oshfs_chmod(const char *path, mode_t mode)
{

#ifdef debugprint
printf("[debug] chmod : %s, mode = %d\n", path, mode);
#endif // debugprint

	char *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
	if((*(Blockkind *)(node + KINDOFFSET)) == efile) {
		(*(struct stat *)(node + FILESTOFFSET)).st_mode = S_IFREG | mode;
	}
	else {
		(*(struct stat *)(node + DIRSTOFFSET)).st_mode = S_IFDIR | mode;
	}

#ifdef debugprint
printf("        kind =  %s\n", (*(Blockkind *)(node + KINDOFFSET)) == efile? "efile" : "edir");
#endif // debugprint

	return 0;
}

static int oshfs_chown(const char *path, uid_t uid, gid_t gid)
{

#ifdef debugprint
printf("[debug] chown\n");
#endif // debugprint

	char *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
#ifdef fastmode
	(*(struct stat *)(node + FILESTOFFSET)).st_uid = uid;
	(*(struct stat *)(node + FILESTOFFSET)).st_gid = gid;
#else
	if((*(Blockkind *)(node + KINDOFFSET)) == efile) {
		(*(struct stat *)(node + FILESTOFFSET)).st_uid = uid;
		(*(struct stat *)(node + FILESTOFFSET)).st_gid = gid;
	}
	else {
		(*(struct stat *)(node + DIRSTOFFSET)).st_uid = uid;
		(*(struct stat *)(node + DIRSTOFFSET)).st_gid = gid;
	}
#endif // fastmode
	return 0;
}

static void oshfs_destroy(void *p){

#ifdef debugprint
printf("[debug] destroy\n");
#endif // debugprint

	destroyBlockNodeAndChildren(root);
	destroyQueue(&Q);
	*(char **)(mem[0]) = root = NULL;
}

static int oshfs_utime (const char *path, struct utimbuf *p){
	char *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
#ifdef fastmode
	(*(struct stat *)(node + FILESTOFFSET)).st_atime = p -> actime;
	(*(struct stat *)(node + FILESTOFFSET)).st_mtime = p -> modtime;
#else
    if((*(Blockkind *)(node + KINDOFFSET)) == efile){
		(*(struct stat *)(node + FILESTOFFSET)).st_atime = p -> actime;
		(*(struct stat *)(node + FILESTOFFSET)).st_mtime = p -> modtime;
    }
    else {
		(*(struct stat *)(node + DIRSTOFFSET)).st_atime = p -> actime;
		(*(struct stat *)(node + DIRSTOFFSET)).st_mtime = p -> modtime;
    }
#endif // fastmode
    return 0;
}

//int(* 	releasedir )(const char *, struct fuse_file_info *)???

//int(* 	fsyncdir )(const char *, int, struct fuse_file_info *)???

static const struct fuse_operations op = {
    .init = oshfs_init,
    .getattr = oshfs_getattr,
    .readdir = oshfs_readdir,
    .mknod = oshfs_mknod,
    .open = oshfs_open,
    .write = oshfs_write,
    .truncate = oshfs_truncate,
    .read = oshfs_read,
    .unlink = oshfs_unlink,
    .mkdir = oshfs_mkdir,
    .rmdir = oshfs_rmdir,
    .rename = oshfs_rename,
    .chmod = oshfs_chmod,
    .chown = oshfs_chown,
    .opendir = oshfs_opendir,
    .destroy = oshfs_destroy,
    .utime = oshfs_utime

};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &op, NULL);
}
