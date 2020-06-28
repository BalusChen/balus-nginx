# inotify 各种问题

## `IN_IGNORED`

在删除、移动文件的时候经常会接收到这个事件。

>  Watch was removed  explicitly  (`inotify_rm_watch(2)`)  or  automatically  (file  was  deleted,  or filesystem was unmounted). 

## 移动文件至监听树之外

比如我监听了`test`目录和`test/1`文件，然后我分别进行以下两个操作：

### 1

```sh
% cd test
% mv 1 ..
```

此时依次得到`IN_MOVED_FROM`、`IN_MOVE_SELF`和`IN_IGNORED`这三个事件：

* `IN_MOVED_FROM`：由于`test/`目录被监听，所以其下的文件`1`被移出目录就会产生这个事件。
* `IN_MOVE_SELF`：由于`test/1`本身被监听，所以被移出时会产生此事件。
* `IN_IGNORED`：前面已经解释过了

### 2

* ``

```sh
% cd test
% mv 1 ~
```

此时产生了

## 参考

[SO: inotify vim modification](https://stackoverflow.com/questions/13409843/inotify-vim-modification)
