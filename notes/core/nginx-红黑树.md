
# Nginx红黑树
Nginx里面也实现了红黑树，我觉得这算是很难的一种数据结构了。
我开始看了《算法》第四版里头的红黑树实现，按照2-3树的原理来看还比较好看；
但是Nginx里面基本是按照《算法导论》里头的伪代码来实现的，和我看的还有些不同。
虽然知道很难，但其实也是一种很重要的数据结构，只得硬着头皮看了。
但是我觉得光看不一定看得懂，(毕竟算导我也没有看懂，而且头大)，
所以我打算把Nginx里头的实现自己再搞一遍，多多练习加深印象。

## 红黑树的性质

1. 每个节点或者是红色的，或者是黑色的。
2. root节点是黑色的。
3. 每个叶子节点是黑色的。
4. 红色节点的两个子节点都必须是黑色的。
5. 对于每个节点，从该节点到其后代叶节点的简单路径上，均包含相同数目的黑色节点。

## 定义

```c
typedef ngx_uint_t ngx_rbtree_key_t;
typedef ngx_int_t  ngx_rbtree_key_int_t;

typedef struct ngx_rbtree_node_s ngx_rbtree_node_t;

struct ngx_rbtree_node_s {
    ngx_rbtree_key_t       key;
    ngx_rbtree_node_t     *left;
    ngx_rbtree_node_t     *right;
    ngx_rbtree_node_t     *parent;
    u_char                 color;
    u_char                 data;
};

typedef struct ngx_rbtree_s ngx_rbtree_t;

typedef void (*ngx_rbtree_insert_pt) (ngx_rbtree_node_t *root,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);

struct ngx_rbtree_s {
    ngx_rbtree_node_t      *root;
    ngx_rbtree_node_t      *sentinel;
    ngx_rbtree_insert_pt    insert;
};
```

除了节点的结构，其他的和《算法导论》中的结构是一样的。其中，`ngx_rbtree_t`结构中的`sentinel`从名字就可以看出是作哨兵用，所有本应该指向NULL的节点都指向它。

## 操作

定义很简单，难就难在操作，尤其是插入、删除(删除最难)。
这里我打算仔细剖析一下这几个操作。

### 初始化

```c
#define ngx_rbtree_init(tree, s, i)         \
    ngx_rbtree_sentinel_init(s);            \
    (tree)->root = s;                       \
    (tree)->sentinel = s;                   \
    (tree)->insert = i

#define ngx_rbtree_sentinel_init(s)     ngx_rbt_black(s);

#define ngx_rbt_black(node)             ((node)->color = 1)
```

初始化没有什么问题，需要注意的就是哨兵`sentinel`的颜色为黑色。

### 插入

```c
void
ngx_rbtree_insert(ngx_rbtree_t *tree, ngx_rbtree_node_t *node)
{
    // 用二级指针
    ngx_rbtree_node_t  **root, *temp, *sentinel;

    /* a binary tree insert */

    root = &tree->root;
    sentinel = tree->sentinel;

    // 树为空的情况
    if (*root == sentinel) {
        node->parent = NULL;
        node->left = sentinel;
        node->right = sentinel;
        ngx_rbt_black(node);
        *root = node;

        return;
    }

    // 按照普通的二叉搜索树插入方法进行插入
    tree->insert(*root, node, sentinel);

    /* 修正树使之满足红黑树的5个性质 */

    while (node != *root && ngx_rbt_is_red(node->parent)) {

        if (node->parent == node->parent->parent->left) {
            // 叔叔节点
            temp = node->parent->parent->right;

            if (ngx_rbt_is_red(temp)) {
                // 情况1
                // 叔叔也是红色(那么爷爷一定为黑色)
                // 那么就将叔叔和父亲的颜色翻转
                // 并且将爷爷的颜色置为红色，从而将红色向上传(也就是将不平衡向上传)

                ngx_rbt_black(node->parent);
                ngx_rbt_black(temp);
                ngx_rbt_red(node->parent->parent);
                // 下一步从爷爷处开始平衡操作
                node = node->parent->parent;

            } else {
                // NOTE: 这里情况2可以转到情况3
                // 叔叔为黑色
                if (node == node->parent->right) {
                    // (情况2)如果插入的节点是父节点的右儿子
                    node = node->parent;
                    ngx_rbtree_left_rotate(root, sentinel, node);
                }

                // 情况3: 插入的节点是父节点的左儿子
                ngx_rbt_black(node->parent);
                ngx_rbt_red(node->parent->parent);
                ngx_rbtree_right_rotate(root, sentinel, node->parent->parent);
            }

        } else {
            // 和对应的if是镜像情况
            temp = node->parent->parent->left;

            if (ngx_rbt_is_red(temp)) {
                ngx_rbt_black(node->parent);
                ngx_rbt_black(temp);
                ngx_rbt_red(node->parent->parent);
                node = node->parent->parent;

            } else {
                if (node == node->parent->left) {
                    node = node->parent;
                    ngx_rbtree_right_rotate(root, sentinel, node);
                }

                ngx_rbt_black(node->parent);
                ngx_rbt_red(node->parent->parent);
                ngx_rbtree_left_rotate(root, sentinel, node->parent->parent);
            }
        }
    }

    ngx_rbt_black(*root);   // 设置root为黑色
}
```

最难的还是修正方法的理解。我没有想到更好的有助于理解的方法，但是我觉得《算法导论》里面的**初始化、终止、保持**三部曲很有用。

首先我们考虑在调用`ngx_rbtree_insert`例程时红黑树的哪些性质会被破坏:

* 性质1和性质3继续成立：因为新插入的红节点的两个子节点都是哨兵
* 性质5页继续成立：因为插入的节点是红色的
* 性质2可能会被破坏：可能插入的节点变成了root(即插入前树为空)
* 性质4可能会被破坏：可能插入的节点的父节点为红色

在`ngx_rbtree_insert`方法中的`while`循环中，每次迭代都保持3个部分的不变式(即**循环不变式**)：

* 节点`node`是红节点
* 如果`node->parent`是root节点，则`node->parent`是黑节点
* 如果有任何红黑性质被破坏，那么至多只有一条被破坏，要么是性质2，要么是性质4。

#### 具体分析父节点为爷爷节点的左儿子时的3种情况

从while循环中我们可以看出，安装父节点是爷爷节点的左儿子还是右儿子分了两种大情况，这两种大情况是对称的，每种大情况里面又分了3中小情况。这里把**父节点为爷爷节点的左儿子**这种大情况挑出来梳理一下：

情况1的问题是什么呢？情况1是我、父亲以及叔叔都是红色节点(说明爷爷肯定是黑色)，那么可以将我父亲和叔叔变成黑色，而爷爷变成红色。这样就把不平衡性向上传了。然后下一次迭代就从爷爷节点开始。

![case-1](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/DSALGO/tree/Case1-of-RB-INSERT-FIXUP.png)


情况2和情况3的问题是什么呢？自己(即新插入的节点)和父节点是红色的(这个违反了性质4)，而叔叔节点是黑色的，而且由父节点为红色我们知道爷爷为黑色。所以我、父亲、爷爷需要重排，以及颜色改变。

1. 首先说重排，该怎么重排呢？其实可以把我、父亲和爷爷三者大小为中间的结点作为新生成的子树的根。而已知爷爷一定是最大的(因为父亲是他的左儿子)，所以这个新子树的root一定是从我和父亲二者之间选出的，爷爷直接作为新子树root的右儿子就好了。那么现在的问题就是，我和父亲谁才是较大者？如果我是父亲的右儿子，那么我就是较大者；可如果我是父亲的左儿子，那么父亲才是较大者。而case-2和case-3就是对这两种情况的讨论。

    * 如果是情况2，我是父亲的右儿子，《算法导论》里面是将这种情况转换为情况3，这只需要以父亲为轴左旋就可以达到目的。
    * 如果是情况3，我是父亲的左儿子，那么我、父亲和爷爷就连成往左下方斜的一条线了，直接以父亲为轴右旋就好了。

2. 然后是颜色改变。情况2在转到情况3之前是没有颜色改变的，所以颜色改变是在情况3中发生的。已知我和父亲都是红色，而爷爷一定是黑色，叔叔也是黑色。而且需要注意的是$\alpha$, $\beta$, $\gamma$和$\delta$子树都必须有一个黑色的root(其中$\alpha$, $\beta$, $\gamma$是通过性质4而来的，而$\delta$的子树root若为红色，则导致的是case-1)。我们为了修正性质4，保持性质5，我们得把新子树的root(由我活着父亲而来，这两个都是红色)变成黑色，然后把右旋之前的爷爷变成红色。

![case-2-and-case3](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/DSALGO/tree/Case2-3-of-RB-INSERT-FIXUP.png)

旋转差不多可以理解了。那么现在的问题就是，这样真的能够解决问题吗？让我们来看看它修正了哪些性质：

`tree->insert`需要调用`ngx_rbtree_node_t`结构中的函数指针进行插入，这方便用户自定义插入操作。但是对于大多数普通的对象而言，经典的二叉搜索树插入操作就可以满足我们的需求，而NGINX也为我们预置了该插入方法:

```c
void
ngx_rbtree_insert_value(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t  **p;

    for ( ;; ) {

        // 根据插入的值的大小决定是插入到左子树还是右子树
        p = (node->key < temp->key) ? &temp->left : &temp->right;

        // 遇到了可插入位置
        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel; ngx_rbt_red(node);  // 新插入的节点的颜色都是红色，后面会按需修正
}
```

很大的的一个特定就是使用了二级指针，立马变得高大上。(不光二级，NGINX连4级指针都用，简直是丧心病狂)

这里也有一个问题(算导里面也提到了)，就是为什么插入一个新的元素时就把这个元素的颜色设置为黑色呢？红色有什么不好呢？


#### 插入的运行时间分析

### 删除

和许多数据结构一样，删除操作总是会比插入操作要难。

不过还好红黑树再怎么难还是一种二叉搜索树，所以其删除还是和普通的BST类似，而难就难在删除节点之后如何恢复红黑树的性质。

* 首先是进行删除操作，这一步和普通的BST是相同的(但是有一点不同的是，传给删除函数的是已经是一个节点指针了，而不是一个key，所以不需要我们再进行查找了)

```c
void
ngx_rbtree_delete(ngx_rbtree__t *tree, ngx_rbtree_node_t *node)
{
    ngx_uint_t           red;
    ngx_rbtree_node_t  **root, *sentinel, *subst, *temp, *w;

    root = &tree->root;
    sentinel = tree->sentinel;

    if (node->left = sentinel) {
        // 要删除的左节点不存在，则用右结点来替换要删除的结点
        temp = node->right;
        subst = node;
    } else if (node->right == sentinel) {
        // 要删除的右节点存在而左节点存在，则用左节点来替换要删除的节点
        temp = node->left;
        subst = node;
    } else {
        // 左右节点都存在，则使用要删除节点的右子树的最小节点来替换之
        // 这一点和BST是一样的
        subst = ngx_rbtree_min(node->right, sentinel);

        if (subst->left != sentinel) {
            // TODO: 不懂为什么，subst已经是右子树最小的节点了，其left一定会等于sentinel吧？
            // 怎么还需要检查
            temp = subst->left;
        } else {
            temp = subst->right;
        }
    }
```

上面这段代码还是比较容易理解的，其实就是BST的删除操作。
由于NGINX红黑树的代码都是从《算法导论》中而来，所以其实里面的许多变量都可以一一对应的：

* `node`: 即书中的`x`，指向要删除的节点
* `subst`: 即为书中的`y`，始终指向要从树中删除的结点或者将要移入树内的结点
    * 当`node`的左右子树有一个为空时，`subst`就等于`node`(也就是指向要删除的结点)
    * 当`node`的左右子树都不为空时，`subst`就指向要用来替换`node`的那个结点(也就是`node`右子树的最小结点)
* `temp`: 指向将要用来替换要删除结点的那个结点
* `w`: 这个和书中的名字一样，
* `red`: 即书中的`y-original-color`，始终

而且`temp`和`subst`是有关联的:

* 如果`subst`是指向要删除的那个结点(也就是`node`)，那么`temp`就是要删除完用来占据该位置的节点
* 如果`subst`是指向要用来替换`node`那个结点，那么等`temp`指向的节点就要占据`subst`的位置


2. 然后检查要删除的是不是`root`结点。

如果`subst`为root结点，那么它肯定不是通过`ngx_rbtree_min`得到的，这就说明`root`结点至少有一个结点为空，而`temp`就指向root的另外一个子节点(当然它也有可能为空)。所以直接用它来替换`root`即可，并改变颜色。

```c
    if (subst == *root) {
        // 要删除的节点为root
        *root = temp;
        ngx_rbt_black(temp);

        /* DEBUG stuff */
        node->left = NULL;
        node->right = NULL;
        node->parent = NULL;
        node->key = 0;

        return;
    }
```


* 然后进行

```c
    // 记录下颜色
    // 这是是否要进行修正的依据
    red = ngx_rbt_is_red(subst);

    if (subst == subst->parent->left) {
        // temp将用来替换subst
        subst->parent->left = temp
    } else {
        subst->parent->right = temp;
    }

    if (subst == node) {
        // subst == node说明node的左右子树至少有一个为空
        // 此时temp就指向另外一个
        temp->parent->right = subst->parent;
    } else {
        // 说明`subst`是要移入树中的节点(也就是用来替换`node`)

        // NOTE: 这个if-else需要注意
        if (subst->parent == node) {
            // 本来在`subst`替换`node`之后，temp就要占据`subst`的位置，其`parent`字段就得指向原来`subst`的父节点
            // 但是如果subst恰好是`node`的子节点，所以`subst`替换`node`之后，`subst`就称为了原来`subst`的父节点
            temp->parent = subst;
        } else {
            // NOTE: 如常
            temp->parent = subst->parent;
        }

        // 用subst替换node
        subst->left = node-left;
        subst->right = node->right;
        subst->parent = node->parent;
        ngx_rbt_copy_color(subst, node);

        if (node == *root) {
            *root = subst;
        } else {
            if (node == node->parent-Left) {
                node->parent->left == subst;
            } else {
                node->parent->right = subst;
            }
        }

        if (subst->left != sentinel) {
            subst->left->parent = subst;
        } else if (subst->right != sentinel) {
            subst->right->parent = subst;
        }
    }

    /* DEBUG stuff */
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    node->key = 0;

```

3. 然后开始修复操作


```c
    if (red) {
        return;
    }
```

上面是判断是否需要修复，从代码中知道`red`是`subst`的颜色，而`subst`指向要删除的结点或者要移入树内的节点。
如果是红色，那么`subst`被删除或者移动时，红黑树的性质没有变化:

1. 树中的黑高没有变化
2. 不存在两个相连接的红节点，因为`subst`占据了`node`的位置，考虑到`node`的颜色，
    树中`subst`的新位置不可能有两个相邻的红节点。
    此外，如果`subst`不是`node`的子节点，那么`subst`原来的右子节点`temp`代替`subst`。
    如果`subst`为红色，则`temp`一定为黑色，一次用`temp`代替`subst`不会使两个红节点相邻
3. 如果`subst`为红色，就不可能为`root`节点，所以根节点为黑色这条性质没有被破坏

如果是黑色，那么就会产生3个问题:

1. 如果`subst`是原来的根节点，而`subst`的一个红色孩子成为了新的节点，那么就违反了性质-2
2. 如果`temp`和`temp.parent`是红色的，那么就违反了性质-4
3. 在树中移动`subst`会导致先前包含`subst`的任何简单路径上的黑节点个数少了1，因此`subst`的任何祖先都不满足性质-5

那么该如何解决呢？

然后开始真正的修复操作：

```c
    while (temp != *root && ngx_rbt_is_black(temp)) {
        if (temp = temp->parent->left) {
            w = temp->parent->right;    // 兄弟节点

            if (ngx_rbt_is_red(w)) {
                // NOTE: case-1，兄弟节点为红色
                ngx_rbt_black(w);
                ngx_rbt_red(temp->parent);
                ngx_rbtree_left_rotate(root, sentinel, temp->parent);
            }

            if (ngx_rbt_is_black(w->left) && ngx_rbt_is_black(w->right)) {
                // NOTE: case-2，兄弟结点w为黑色，且w的两个子节点都为黑色
                ngx_rbt_red(w);
                temp = temp->parent;
            } else {
                if (ngx_rbt_is_black(w->right)) {
                    // NOTE: case-3，兄弟结点为黑色，w的左子节点为
                    ngx_rbt_black(w->left);
                    ngx_rbt_red(w);
                    ngx_rbtree_right_rotate(root, sentinel, w);
                    w = temp->parent->right;
                }
            }
        }
    }
```

![rbtree-delete-fixup](https://raw.githubusercontent.com/JianYongChan/Markdown_Photos/master/Nginx/ngx-rbtree-delete-fixup.png)


## 参考

[geeks-for-geeks: rbtree insert](https://www.geeksforgeeks.org/red-black-tree-set-2-insert/)

[geeks-for-geeks: rbtree delete](https://www.geeksforgeeks.org/red-black-tree-set-3-delete-2/)

[why-makes-nodes-red-when-inserted](https://stackoverflow.com/questions/15451456/red-black-tree-inserting-why-make-nodes-red-when-inserted)

