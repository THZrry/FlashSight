使用WhaleUI搭建一个GUI的code agent
WhaleUI（github.com/THZrry/WhaleUI,本地在C:/aaa/WhaleUI）是一个无JS的网页渲染器框架，用于支撑本项目的界面。
xmake用于管理项目
使用C++14完成项目
使用C/C++的轻量markdown解析器（最好支持增量解析）并直接得到HTML元素和css，再在diff下更新界面，以达到传统electron agent无法得到的高性能
reproc，进行子进程管理