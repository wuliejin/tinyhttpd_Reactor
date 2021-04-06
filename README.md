<!--
 * @Descripttion: 
 * @version: 
 * @Author: wuliejin
 * @Date: 2021-04-07 00:31:58
 * @LastEditors: wuliejin
 * @LastEditTime: 2021-04-07 00:32:18
-->
# tinyhttpd_Reactor
基于tinyhttpd实现的项目，加入了基于Reactor模型实现的IO复用

# 项目背景
为了加深对linux环境下编程、socket编程、IO复用模型等知识的理解，在参考tinyhttpd这一经典项目的基础上实现了这一服务器，在原有项目的基础上加入了多线程的设计，实现了高并发的http请求处理

# 安装
'''
git clone https://github.com/wuliejin/tinyhttpd_Reactor.git
make clean
make
'''

# 使用
'''
./httpd_string
'''

# 

# 不足
epoll这一设计
