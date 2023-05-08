# FTP-server in С
## _Description_

This is a multithreaded FTP server with the ability to request directories and transfer them in archived form. Here are the client and server code. The server side uses: PTHREAD, SQLite. The client side uses: PTHREAD.

## _Requests_

In the client part, you can use the following queries:

| Request | Description | Expected code |
| ------ | ------ | ------ |
| USER <username> | for authorization | 331 |
| PASS <password> | must be right after USER (if not anonymous) | 230 |
| PORT <ip1,ip2,ip3,ip4,port1, port2> | to establish an active connection | 200 |
| PASV | to establish a passive connection | 227 |
| LIST <dir> | list of files and directories in the folder | 150 | 
| MKD <dir> | create a folder | 257 |
| QUIT | exit | 221 |
| DELE <filename> | delete file | 250 |
| CWD <dir> | change the working directory | 250 |
| PWD <dir> | print the working directory | 257 |
| RETR <filename> | download file | 226 |
| STOR <filename> | upload a file | 226 |
| RNFR <filename> | select a file to rename | 350 |
| RNTO <filename> | rename a file | 250 |
| SIZE <filename> | get file size | 230 |
| ABOR | Cancel the transfer | 226 |
| RMD <dir> | delete a folder | 250 |
| TYPE <type> | set the transfer type | 200 |
| HELP | get information | 215 |
| NOOP | to check | 200 |

## _Run_

You must run the client and server parts separately using the command:
``` make run```