#实验三 文件系统

 - 实现了基本的目录操作
 - 可以修改该部分文件属性
 - 有错误处理程序，能报告大部分错误
 
##限制和要求

```c
//#define debugprint

#define test2 4096		//如果printf输出了 “请把test2修改为xxx”，在这里修改即可

#define MAXSIZE (2 * 1024 * 1024 * (size_t)1024)     //2GB
#define BLOCKSIZE (4 * (size_t)1024)                 //4KB	注：不要修改！
#define MAXNAMELEN 256								//文件名最长255个字符
#define PATHMAXLEN 4096								//路径名最长4095个字符
```

 - 总空间大小默认设置为2GB，可以调大。
 - 每块大小设置为4KB，强烈建议不要作任何调整，因为后面有专门的程序加速大文件写入，而修改后加速程序不会生效，而且可能出问题。
 -  文件名最长255个字符
 -  完整路径名不能超过4095个字符
 -  文件个数不能无限多，因为用递归释放空间，要保证不会栈溢出。（文件大小不限制）
 -  如果有问题，打开debugprint并加上-f运行，如果再init中输出“请把test2修改为xxx”，就按提示修改即可。（结构体成员中包含void *指针和字符数组时，总大小和各个成员大小之和不等，不知道为什么）

##数据结构和算法

###广义表
类似于广义表，采用扩展线性链表存储。三种结点：文件结点、文件夹结点、数据结点，其中构成广义表的可以认为只有前两种（文件结点 - 原子结点 ，文件夹结点 - 表结点）

![GList](https://raw.githubusercontent.com/OSH-2018/3-yxyyyxxyy/master/pic/GList.PNG)

这里每个结构体要与4k对齐，content域的大小并非4k - 其他域大小（不知道为什么），在我电脑上设置为4096 - 8 - sizeof其他成员才可以对齐。如果无法正确运行可以尝试修改（test2宏定义）。除了第一个枚举变量，下面的是共用体。

为了效率更高，用顺序广义表，按先文件夹后文件、其次字母顺序存储，搜索时可以按路径搜索，插入为有序插入，删除文件夹时采用递归（内容太多会栈溢出报错，没有考虑这个）。

每一块进行动态分配和释放。

###大文件写入

我发现大文件写入都是调用很多次write，每次只写4k，每次的offset差4k。这样我通过记录上一次调用的信息，当满足这个条件时视为命中，则无需重新搜索path对应的结点，也不用搜索offset处的节点，基本上可以随机写入。**所以BLOCKSIZE不要修改**。

![Write](https://raw.githubusercontent.com/OSH-2018/3-yxyyyxxyy/master/pic/write.png)

##扩展内容

###目录操作、部分文件属性、错误处理
![Result](https://raw.githubusercontent.com/OSH-2018/3-yxyyyxxyy/master/pic/result.png)
