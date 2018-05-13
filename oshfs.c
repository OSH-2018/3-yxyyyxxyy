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

#define test2 4096			//如果printf输出了 “请把test2修改为xxx”，在这里修改即可

#define min(a,b) ((a) < (b) ? (a) : (b))

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

typedef struct filenode {
    char filename[MAXNAMELEN];
    struct stat filest;
    void *next;
    void *nextcontent;

#if test2 == 4096
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2 - 8];		//能写一部分东西
#elif test2 == 4092
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2 - 4];
#else
	char content[BLOCKSIZE - sizeof(Blockkind)- sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2];
#endif	//test2

}Filenode;

typedef struct dirnode{
	char dirname[MAXNAMELEN];
	struct stat dirst;
	void *next;
	void *firstchild;

#if test2 == 4096
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2 - 8];
#elif test2 == 4092
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2 - 4];
#else
	char notuse[BLOCKSIZE - sizeof(Blockkind) - sizeof(char) * MAXNAMELEN - sizeof(struct stat) - sizeof(void *) * 2];
#endif	//test2

}Dirnode;

typedef struct datanode{
	void *nextcontent;

#if test2 == 4096
	char content[BLOCKSIZE - sizeof(Blockkind) - sizeof(void *) - 8];
#elif test2 == 4092
	char content[BLOCKSIZE - sizeof(Blockkind) - sizeof(void *) - 4];
#else
	char content[BLOCKSIZE - sizeof(Blockkind) - sizeof(void *)];
#endif // test2

}Datanode;

typedef struct blocknode{
	Blockkind kind;
	union{
		Filenode file;
		Dirnode dir;
		Datanode data;
	};
}Blocknode;


Blocknode testsize;
static const size_t datasize = sizeof(testsize.data.content);	//constant
static const size_t filesize = sizeof(testsize.file.content);	//constant

/************************************辅助函数、全局变量*************************************/

static unsigned int blockuse = 0;		//已使用的块数

static Blocknode *root = NULL, *last = NULL, *tail = NULL;
static off_t lastoffset = 0, lastsize = 0;		//相当于一个cache用于提升write的速度。write总是4k4k的写
static char lastpath[256];				//用于cache

static Blocknode *mallocNode(Blockkind bk){
	if(blockuse >= blocknr) return NULL;		//已经满了 失败
	blockuse ++;
	Blocknode *p = (Blocknode *)mmap(NULL, blocksize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(p, 0, blocksize);
	p -> kind = bk;

#ifdef debugprint
	printf("malloc node pointer is %p \n", p);
#endif // debugprint

	return p;
}

static void freeNode(Blocknode *p){
	if(p) {
		munmap(p, blocksize);
		blockuse --;
	}
}

char *stringSplit(char **s, char c){  //找到字符串s中第一次出现的c，返回前一半，s指向后一半,长度小于PATHMAXLEN
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

static void traverseBlockInSameLayer(Blocknode *first){
	printf("[debug] ");
	Blocknode *t = first;
	while(t){
		printf("%s ", (t -> kind == efile) ? (t -> file).filename : (t -> dir).dirname);
		t = (Blocknode *)((t -> kind == efile) ? (t -> file).next : (t -> dir).next);
	}
	printf("\n");
}

static void printNodeInfo(Blocknode *node){
	if(!node) {
		printf("[NodeInfo]\tnode is null\n");
		return;
	}
	switch(node -> kind){
		case efile: printf("[NodeInfo]\tnode kind is %s\n\t\tnode name is %s\n", "file", (node -> file).filename);break;
		case edir: printf("[NodeInfo]\tnode kind is %s\n\t\tnode name is %s\n", "dir", (node -> dir).dirname);break;
		case edata: printf("[NodeInfo]\tnode kind is data\n");break;
	}
}

#endif // debugprint

static Blocknode *getBlockNode(const char *name, int stopAtParent, char **filename)
{
	char *path = (char *)((name && *name == '/') ? name + 1 : name), *str = NULL;					//跳过开头的 /
    Blocknode *p = root, *q = NULL;
    while(path && *path && (p || stopAtParent)){
		str = stringSplit(&path, '/');
		if(!path || !*path){		//str表示文件
			if(stopAtParent){
				*filename = str;
				return q;
			}
			while(p){
				int f = 1;
				while(p && p -> kind == edir && (f = strcmp((p -> dir).dirname, str))) p = (Blocknode *)((p -> dir).next);	//找下一个
				while(p && p -> kind == efile && (f = strcmp((p -> file).filename, str))) p = (Blocknode *)((p -> file).next);
				if(!f) return p;	//找到
			}
			return NULL;	//没找到文件
		}
		else{		//进入文件夹
			while(p){
				while(p && p -> kind == efile) p = (Blocknode *)((p -> file).next);	//找到同目录下一个文件夹
				while(p && p -> kind == edir && strcmp((p -> dir).dirname, str)) p = (Blocknode *)((p -> dir).next);
				if(p && p -> kind == edir && !strcmp((p -> dir).dirname, str)) {	//找到了
					q = p;
					p = (Blocknode *)((p -> dir).firstchild);
					goto finddir;
				}
			}
			return NULL;	//没找到文件夹
		}
		finddir: ;		//找到了文件夹，继续
    }
    return NULL;
}


static int createBlockNode(const char *path, const struct stat *st, Blockkind kind)		//争取先文件夹再文件 按字母顺序存储
{
    Blocknode *p, *q, *r;
    char *str = NULL;
	r = q = p = getBlockNode(path, 1, &str);
    if(r){	//不再根目录
		p = (Blocknode *)((p -> dir).firstchild);
		int f = 1;
		while(p && p -> kind == edir && (f = strcmp((p -> dir).dirname, str))){
			if(kind == edir) if(f > 0) break;
			q = p;		//记录前一个
			p = (Blocknode *)((p -> dir).next);	//找下一个
		}
		while(p && p -> kind == efile && (f = strcmp((p -> file).filename, str))){
			if(kind == efile) if(f > 0)break;
			if(kind == edir) break;		//创建文件夹 应该放在文件前面
			q = p;
			p = (Blocknode *)((p -> file).next);
		}
		if(!f) return 1;	//文件已存在 1
		if(kind == edir){		//找有没有重名文件
			Blocknode *s = p;
			while(s && s -> kind == edir && (f = strcmp((s -> dir).dirname, str))){
				s = (Blocknode *)((s -> dir).next);	//找下一个
			}
			while(s && s -> kind == efile && (f = strcmp((s -> file).filename, str))){
				s = (Blocknode *)((s -> file).next);
			}
			if(!f) return 1;	//文件已存在 1
		}

		//创建节点
		if(strlen(str) >= MAXNAMELEN - 1) return 4;		//name too long
		Blocknode *n = mallocNode(kind);
		if(!n) return 3;	//文件系统满了 3
		if(kind == efile){
			strcpy((n -> file).filename, str);
			(n -> file).filest = *st;
			(n -> file).nextcontent = NULL;
			(n -> file).next = NULL;
		}
		else {
			strcpy((n -> dir).dirname, str);
			(n -> dir).dirst = *st;
			(n -> dir).firstchild = NULL;
			(n -> dir).next = NULL;
		}

		//插入顺序链表
		if(r == q){		//r是空文件夹或应该存在第一个位置
			void *temp = (r -> dir).firstchild;
			(r -> dir).firstchild = (void *)n;
			if(kind == efile) (n -> file).next = temp;
			else (n -> dir).next = temp;
		}
		else if(q -> kind == edir){		//r不是空文件夹
			void *temp = (q -> dir).next;
			(q -> dir).next = (void *)n;
			if(kind == efile) (n -> file).next = temp;
			else (n -> dir).next = temp;
		}
		else{
			void *temp = (q -> file).next;
			(q -> file).next = (void *)n;
			if(kind == efile) (n -> file).next = temp;
			else (n -> dir).next = temp;
		}
	}
	else{		//在根目录
		if(*path != '/')return 2;		//目录不存在 2
		p = root, q = NULL;
		int f = 1;
		while(p && p -> kind == edir && (f = strcmp((p -> dir).dirname, str))){
			if(kind == edir) if(f > 0) break;
			q = p;		//记录前一个
			p = (Blocknode *)((p -> dir).next);	//找下一个
		}
		while(p && p -> kind == efile && (f = strcmp((p -> file).filename, str))){
			if(kind == efile) if(f > 0)break;
			if(kind == edir) break;
			q = p;
			p = (Blocknode *)((p -> file).next);
		}
		if(!f) return 1;	//文件已存在 1
		if(kind == edir){		//找有没有重名文件
			Blocknode *s = p;
			while(s && s -> kind == edir && (f = strcmp((s -> dir).dirname, str))){
				s = (Blocknode *)((s -> dir).next);	//找下一个
			}
			while(s && s -> kind == efile && (f = strcmp((s -> file).filename, str))){
				s = (Blocknode *)((s -> file).next);
			}
			if(!f) return 1;	//文件已存在 1
		}

		//创建节点
		if(strlen(str) >= MAXNAMELEN - 1) return 4;		//name too long
		Blocknode *n = mallocNode(kind);
		if(!n) return 3;	//文件系统满了 3
		if(kind == efile){
			strcpy((n -> file).filename, str);
			(n -> file).filest = *st;
			(n -> file).nextcontent = NULL;
			(n -> file).next = NULL;
		}
		else {
			strcpy((n -> dir).dirname, str);
			(n -> dir).dirst = *st;
			(n -> dir).firstchild = NULL;
			(n -> dir).next = NULL;
		}

		//插入顺序链表
		if(!q){		//要插在root
			Blocknode *temp = root;
			root = n;
			if(kind == efile) (n -> file).next = (void *)temp;
			else (n -> dir).next = (void *)temp;
		}
		else if(q -> kind == edir){		//r不是空文件夹
			void *temp = (q -> dir).next;
			(q -> dir).next = (void *)n;
			if(kind == efile) (n -> file).next = temp;
			else (n -> dir).next = temp;
		}
		else{
			void *temp = (q -> file).next;
			(q -> file).next = (void *)n;
			if(kind == efile) (n -> file).next = temp;
			else (n -> dir).next = temp;
		}
	}

#ifdef debugprint
	traverseBlockInSameLayer(root);
#endif // debugprint

	return 0;	//0成功
}

static int deleteBlockNode(const char *path){//touch a && mkdir -p b/bb && mkdir -p c/cc	bb会消失 c会消失
	char *str;
	int f = 0;
	Blocknode *r = getBlockNode(path, 1, &str), *q = NULL, *p = NULL;
	if(!r) p = root;
	else p = (Blocknode *)((r -> dir).firstchild);

	while(p && p -> kind == edir && (f = strcmp((p -> dir).dirname, str))) {
		q = p;
		p = (Blocknode *)((p -> dir).next);	//找下一个
	}
	while(p && p -> kind == efile && (f = strcmp((p -> file).filename, str))) {
		q = p;
		p = (Blocknode *)((p -> file).next);
	}
	if(!p){
		return 1;	//file or dir not found 1
	}
	else if(!q){		//the first should be deleted
		if((p -> kind) == edir) {
			if(p == root) {
				root = (p -> dir).next;
				freeNode(p);
			}
			else {
				(r -> dir).firstchild = (p -> dir).next;
				freeNode(p);
			}
		}
		else {
			if(p == root) {
				root = (p -> file).next;
				freeNode(p);
			}
			else {
				(r -> dir).firstchild = (p -> file).next;
				freeNode(p);
			}
		}
	}
	else if((p -> kind) == edir && (q -> kind) == edir) {
		(q -> dir).next = (p -> dir).next;
		freeNode(p);
	}
	else if((p -> kind) == efile && (q -> kind) == edir) {
		(q -> dir).next = (p -> file).next;
		freeNode(p);
	}
	else {
		(q -> file).next = (p -> file).next;
		freeNode(p);
	}
	return 0;	//succeed
}

static void destroyBlockDataLinkList(Blocknode *node){
	Blocknode *p = node, *q = node;
	while(p){
		q = (Blocknode *)((p -> data).nextcontent);
		freeNode(p);
		p = q;
	}
}

static size_t getCurrentFileContentSize(Blocknode *node){
	Blocknode *p = node;
	size_t origsize = 0;
	while(p){
		if((p -> kind == efile)) {
			origsize += sizeof((p -> file).content);
			p = (p -> file).nextcontent;
		}
		else if((p -> kind == edata)) {
			origsize += sizeof((p -> data).content);
			p = (p -> data).nextcontent;
		}
	}
	return origsize;
}

static int modifyCurrentFileContentSize(Blocknode *node, size_t targetSize){
	Blocknode *p = node, *q = NULL;
	size_t origsize = 0;
	while(p){
		if((p -> kind == efile)) {
			origsize += sizeof((p -> file).content);
			if(origsize >= targetSize){
				destroyBlockDataLinkList((Blocknode *)((p -> file).nextcontent));
				(p -> file).nextcontent = NULL;
			}
			q = p;
			p = (Blocknode *)((p -> file).nextcontent);
		}
		else if((p -> kind == edata)) {
			origsize += sizeof((p -> data).content);
			if(origsize >= targetSize){
				destroyBlockDataLinkList((Blocknode *)((p -> data).nextcontent));
				(p -> data).nextcontent = NULL;
			}
			q = p;
			p = (Blocknode *)((p -> data).nextcontent);
		}
	}
	tail = q;
	off_t need = targetSize - origsize;
	lastsize = -need;
	if(need <= 0)return 0;
	if(need / sizeof((q -> data).content) + blockuse + 1 > blocknr)return 1;		//空间不足
	while(origsize < targetSize){
		if(q -> kind == efile){
			(q -> file).nextcontent = (void *)mallocNode(edata);
			q = (Blocknode *)((q -> file).nextcontent);
		}
		else if(q -> kind == edata){
			(q -> data).nextcontent = (void *)mallocNode(edata);
			q = (Blocknode *)((q -> data).nextcontent);
		}
		if(!q)return 1;
		(q -> data).nextcontent = NULL;
		origsize += sizeof((q -> data).content);
	}
	lastsize = origsize - targetSize;	//还差多少满
	tail = q;
	return 0;
}

static int adjustFileContent(Blocknode *node, off_t offset, const char *buf, size_t size){	//在node文件中offset处写入大小为size的buf
	int hit = 0;
	Blocknode *save;
	size_t hitoffset;
	if(cachemode && last == node && size == 4096 && offset - lastoffset == 4096)	//cache
		{	//hit

#ifdef debugprint
			printf("       hit! name is %s\n", (node -> file).filename);
#endif // debugprint
			hit = 1;
			save = tail;
			hitoffset = (tail -> kind == efile ? filesize : datasize) - lastsize;
			if(lastsize + datasize > 4096){	//need one block
				if(tail -> kind == efile){
					(tail -> file).nextcontent = (void *)mallocNode(edata);
					tail = (Blocknode *)((tail -> file).nextcontent);
				}
				else {
					(tail -> data).nextcontent = (void *)mallocNode(edata);
					tail = (Blocknode *)((tail -> data).nextcontent);
				}
				if(!tail)return 1;
				(tail -> data).nextcontent = NULL;
				lastsize = lastsize + datasize - 4096;
			}
			else {	//need two blocks
				if(tail -> kind == efile){
					(tail -> file).nextcontent = (void *)mallocNode(edata);
					tail = (Blocknode *)((tail -> file).nextcontent);
				}
				else {
					(tail -> data).nextcontent = (void *)mallocNode(edata);
					tail = (Blocknode *)((tail -> data).nextcontent);
				}
				if(!tail)return 1;
				(tail -> data).nextcontent = (void *)mallocNode(edata);
				tail = (Blocknode *)((tail -> data).nextcontent);
				if(!tail)return 1;
				(tail -> data).nextcontent = NULL;
				lastsize = lastsize + datasize - 4096 * 2;
			}
		}

	else if(modifyCurrentFileContentSize(node, (node -> file).filest.st_size)) return 1;	//空间不足
	last = node;
	lastoffset = offset;
	Blocknode *p = node;
	char *pos = (char *)buf;
	size_t needsize = size;
	off_t needoffset = offset;
	if(hit){
		p = save;
		needoffset = hitoffset;
	}
	while(p){
		if((p -> kind == efile)) {
			if(filesize > needoffset){
				size_t ss = min(filesize - needoffset, needsize);
				memcpy((p -> file).content + needoffset, pos, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= filesize;
			p = (Blocknode *)((p -> file).nextcontent);
		}
		else {
			if(datasize > needoffset){
				size_t ss = min(datasize - needoffset, needsize);
				memcpy((p -> data).content + needoffset, pos, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= datasize;
			p = (Blocknode *)((p -> data).nextcontent);
		}
	}
	return 0;
}

static void readFileContent(Blocknode *node, char *buf, off_t offset, int size){
	Blocknode *p = node;
	char *pos = buf;
	int needsize = size;
	off_t needoffset = offset;
	while(p){
		if((p -> kind == efile)) {
			if(sizeof((p -> file).content) > needoffset){
				size_t ss = min(sizeof((p -> file).content) - needoffset, needsize);
				memcpy(pos, (p -> file).content + needoffset, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= sizeof((p -> file).content);
			p = (p -> file).nextcontent;
		}
		else if((p -> kind == edata)) {
			if(sizeof((p -> data).content) > needoffset){
				size_t ss = min(sizeof((p -> data).content) - needoffset, needsize);
				memcpy(pos, (p -> data).content + needoffset, ss);
				pos += ss;
				needoffset = 0;
				needsize -= ss;
				if(needsize <= 0)break;
			}
			else needoffset -= sizeof((p -> data).content);
			p = (p -> data).nextcontent;
		}
	}
}


void destroyBlockNodeAndChildren(Blocknode *node){	//clear dir
	if(!node)return;
	if((node -> kind) == efile) destroyBlockNodeAndChildren(((Blocknode *)(node -> file).next));
	else {
		destroyBlockNodeAndChildren(((Blocknode *)(node -> dir).next));
		destroyBlockNodeAndChildren(((Blocknode *)(node -> dir).firstchild));
	}

	//delete node
	if((node -> kind) == efile) {
		destroyBlockDataLinkList((Blocknode *)(((node -> file).nextcontent)));	//destroy content
		freeNode((Blocknode *)node);	//destroy filenode
	}
	else {
		freeNode((Blocknode *)node);	//destroy dirnode
	}
}

static void renameBlock(Blocknode *node, const char *newname){
	if(node -> kind == efile){
		strcpy((node -> file).filename, newname);
	}
	else {
		strcpy((node -> dir).dirname, newname);
	}
}

/************************************接口实现*************************************/


static void *oshfs_init(struct fuse_conn_info *conn)
{
#ifdef debugprint
	if(sizeof(testsize) != 4096){
		fprintf(stderr,"请把宏定义的test2值修改为%ld\n",sizeof(testsize));
		getchar();
		getchar();
	}
	printf("[debug] init\n");
#endif // debugprint

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

	return NULL;
}

static int oshfs_getattr(const char *path, struct stat *stbuf)
{

#ifdef debugprint
printf("[debug] getattr\n");
#endif // debugprint

    int ret = 0;
    Blocknode *node = getBlockNode(path, 0, NULL);
    if(strcmp(path, "/") == 0) {
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
    } else if(node) {
        memcpy(stbuf, (node -> kind == efile) ? &(node -> file).filest : &(node -> dir).dirst, sizeof(struct stat));
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

	Blocknode *node = root;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    if(strcmp(path, "/")){
		node = getBlockNode(path, 0, NULL);
		if(!node) return -ENOENT;
		node = (Blocknode *)((node -> dir).firstchild);
    }
    while(node) {
		if(node -> kind == efile){
			filler(buf, (node -> file).filename, &(node -> file).filest, 0);
			node = (Blocknode *)((node -> file).next);
		}
		else {
			filler(buf, (node -> dir).dirname, &(node -> dir).dirst, 0);
			node = (Blocknode *)((node -> dir).next);
		}
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

	Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(node -> kind != efile)return -EISDIR;		//是个文件夹
    return 0;
}


static int oshfs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{			//他一次只调用这个写4k，效率极低

#ifdef debugprint
	printf("[debug] write : path = %s, size = %ld, offset = %ld\n", path, size, offset);
#endif // debugprint

#ifdef debugprint
    if(root) printf("        before write: root -> kind == %s!\n", root -> kind == efile ? "efile" : "edir");
#endif // debugprint

	Blocknode *node;
	if(cachemode && !strcmp(path, lastpath) && last) node = last;
    else {
		node = getBlockNode(path, 0, NULL);
		if(!node) return -ENOENT;		//没有找到文件
		if(node -> kind != efile)return -EISDIR;		//是个文件夹
		strcpy(lastpath, path);
	}
    if((node -> file).filest.st_size < offset + size) (node -> file).filest.st_size = offset + size;
    if(adjustFileContent(node, offset, buf, size)){
		last = NULL;
		return -ENOMEM;
	}

#ifdef debugprint
    if(root) printf("        after write: root -> kind == %s!\n", root -> kind == efile ? "efile" : "edir");
#endif // debugprint

    return size;
}

static int oshfs_truncate(const char *path, off_t size)
{

#ifdef debugprint
printf("[debug] truncate : path = %s , size = %ld\n", path, size);
#endif // debugprint

	last = NULL;
    Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
	if(node -> kind != efile)return -EISDIR;		//是个文件夹
    (node -> file).filest.st_size = size;
    if(modifyCurrentFileContentSize(node, size))return -ENOMEM;	//空间不足
    return 0;
}

static int oshfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

#ifdef debugprint
printf("[debug] read\n");
#endif // debugprint

	last = NULL;
    Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(node -> kind != efile)return -EISDIR;		//是个文件夹
    int ret = size;
    if(offset + size > (node -> file).filest.st_size)
        ret = (node -> file).filest.st_size - offset;
	readFileContent(node, buf, offset, ret);
    return ret;
}

static int oshfs_unlink(const char *path)
{

#ifdef debugprint
printf("[debug] unlink : %s\n", path);
#endif // debugprint

	last = NULL;
	Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if((node -> kind) == edir){
		destroyBlockNodeAndChildren((Blocknode *)((node -> dir).firstchild));	//clear dir's children
		deleteBlockNode(path);
	}
	else{
		destroyBlockDataLinkList((Blocknode *)((node -> file).nextcontent));	//clear content
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

	Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if((node -> kind) == edir){
		if(((node -> dir).firstchild)) return -ENOTEMPTY;
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
	Blocknode *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
    if(node -> kind == efile) return -ENOTDIR;		//是个文件
    return 0;
}

static int oshfs_rename(const char *path, const char *newname)
{

#ifdef debugprint
printf("[debug] rename : %s -> %s\n", path, newname);
#endif // debugprint

	Blocknode *node = getBlockNode(path, 0, NULL), *p;
	char *str, *q = (char *)newname;
	while(q)str = stringSplit(&q, '/');
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

	Blocknode *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
	if(node -> kind == efile) {
		(node -> file).filest.st_mode = S_IFREG | mode;
	}
	else {
		(node -> dir).dirst.st_mode = S_IFDIR | mode;
	}

#ifdef debugprint
printf("        kind =  %s\n", node -> kind == efile? "efile" : "edir");
#endif // debugprint

	return 0;
}

static int oshfs_chown(const char *path, uid_t uid, gid_t gid)
{

#ifdef debugprint
printf("[debug] chown\n");
#endif // debugprint

	Blocknode *node = getBlockNode(path, 0, NULL);
	if(!node) return -ENOENT;		//没有找到文件
	if(node -> kind == efile) {
		(node -> file).filest.st_uid = uid;
		(node -> file).filest.st_gid = gid;
	}
	else {
		(node -> dir).dirst.st_uid = uid;
		(node -> dir).dirst.st_gid = gid;
	}
	return 0;
}

static void oshfs_destroy(void *p){

#ifdef debugprint
printf("[debug] destroy\n");
#endif // debugprint

	destroyBlockNodeAndChildren(root);
	root = NULL;
}

static int oshfs_utime (const char *path, struct utimbuf *p){
	Blocknode *node = getBlockNode(path, 0, NULL);
    if(!node) return -ENOENT;		//没有找到文件
    if(node -> kind == efile){
		(node -> file).filest.st_atime = p -> actime;
		(node -> file).filest.st_mtime = p -> modtime;
    }
    else {
		(node -> dir).dirst.st_atime = p -> actime;
		(node -> dir).dirst.st_mtime = p -> modtime;
    }
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
