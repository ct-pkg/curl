# curl

`libcurl` 的 easy-handle 绑定。对外只有一个对象：`CurlEasy *`。URL 是构造入口，
其余能力全部是句柄上的方法，配置方法一律返回接收者，所以一次请求就是一个表达式：

```ct
CurlEasy *c = "https://example.com"
                  .openCurl()
                  .setTimeout(15)
                  .setFollow(1)
                  .addHeader("Accept", "application/json")
                  .perform();
```

- 版本：0.1.0
- 许可：MIT（`.ct` / shim / 测试）+ curl 原许可（链接的 libcurl）
- 平台：macOS 与 Linux 的系统 libcurl
- 设计稿：[docs/design.md](docs/design.md)

---

## 1. 定位与边界

这个包只做一件事：把 libcurl 的 **easy handle** 映射成 CT 里的链式对象。

它解决的是“发一个 HTTP 请求并读回结果”这条最短路径，而不是给 libcurl 做全量镜像。
判据是：**一个方法能不能只用一次 `curl_easy_setopt` / `curl_easy_getinfo` 表达，
并且不需要调用方再管理第二个对象。** 满足就进，不满足就推迟。

第一期进：easy 句柄的创建、配置、执行、清理；URL、自定义方法、超时、重定向、
TLS 校验、CA、User-Agent、Basic 认证；请求头；POST body；默认响应体 sink；
可选写回调 / 头回调；HTTP 状态码、最终 URL、Content-Type、耗时、下载字节数。

第一期不进：`multi`（异步状态机，与同步链式语法冲突）、`share`、`mime`、`form`
（上游已弃用）、WebSocket、`urlapi`、`mprintf`、公开的 `CurlSlist`。
公开的 `CurlSlist` 之所以推迟，是因为包装壳已经替调用方持有一条内部 header 列表，
`addHeader(name, value)` 直接追加，第一期没有第二个对象要管。

**唯一允许留在 `namespace` 的能力是进程级的**：`globalInit` / `globalCleanup` /
`version` / `strerror`。它们没有可挂载的接收者。任何配置入口都不在 namespace 上。

## 2. 依赖与安装

### 2.1 libcurl

需要 `curl/curl.h` 与 `-lcurl`。macOS 用系统 SDK 自带的 libcurl 即可；
Homebrew 的安装前缀是 `/opt/homebrew/opt/curl/`，**不是** `/opt/homebrew/include`。

最低版本 **7.55.0**，原因是 `CURLINFO_SIZE_DOWNLOAD_T` / `CURLINFO_SIZE_UPLOAD_T`
自 7.55.0 起才有。低于这个版本会在编译期被 `#if LIBCURL_VERSION_NUM` 拦住。

### 2.2 包元数据

`ct.pkg` 里 `ldflags = -lcurl`，`ct install` 会把它拼进打印出来的命令行。
`ct build` 不读 `ct.pkg`，所以要手工带：

```sh
ct build -I <pkg-root> --ldflags '-lcurl' your.ct -o your
```

### 2.3 编译器前提（重要）

`ct.pkg` 声明 `requires = ct >= 0.20.3`。

原因是本包用了类型别名：

```ct
pub type CurlEasy = struct ct_curl_easy;
```

这是 Phase 20 的 `pub type` 特性。**0.20.2（`0263cca`）里还没有它**，别名将随
**0.20.3** 发布。也就是说：

- 已发布版本**编不过**这个包，会在 `pub type` 那一行报
  `error: use of an undeclared variable`；
- 在 0.20.3 发布前，请用带 alias 改动的工作树自举出的 `ct` 构建；
- 本仓库的验证用的是这样一个工作树构建，`ct --version` 自报 `v0.20.1`——
  版本号低不代表它缺这个特性，它包含未发布的 alias 改动。

字面量作接收者（`"https://x".openCurl()`）是另一件事，那个能力 0.20.0 就发布了。

### 2.4 安装

```sh
ct install ./curl          # 读 ct.pkg，把 -lcurl 一起记下来
```

## 3. 快速开始

```sh
ct build -I ./curl --ldflags '-lcurl' demo/main.ct -o demo/demo
./demo/demo                          # 默认 https://example.com
./demo/demo http://127.0.0.1:8080/   # 也可以指定任意 URL

bash tests/run.sh                    # 全套测试（自带 loopback 夹具服务）
CT=/path/to/ct bash tests/run.sh     # 指定编译器
```

最小程序：

```ct
#include <stdio.h>
import curl::Client;

int main(void)
{
    CurlEasy *c;

    curl::Lib.globalInit();

    c = "https://example.com".openCurl().setTimeout(15).setFollow(1).perform();
    if (c.status() != 0) {
        printf("error %d: %s\n", c.status(), c.message());
    } else {
        printf("HTTP %d, %d bytes\n", c.responseCode(), c.bodyLen());
    }
    c.destroy();

    curl::Lib.globalCleanup();
    return 0;
}
```

## 4. API 全表

记号：**链式 ✓** 表示返回接收者本身（`return c;`），可以继续接方法；
**—** 表示终止或纯读取。`CurlEasy *` 上的 33 个方法里有 21 个可入链，
加上链头 `openCurl`，全包一共 22 个方法返回接收者。

### 4.1 宿主 `char *`：trait `CurlEntry`

| 宿主 | 方法 | 说明 | 参数 | 返回 | 链式 |
| --- | --- | --- | --- | --- | --- |
| `char *` | `openCurl` | 用 URL 创建 easy 句柄；失败返回 0 | — | `CurlEasy *` | 链头 |

`char *` 上**只有**这一个方法。`openCurl` 已核对 `docs/char-host-methods.md`，
不与既有 37 个 `char *` 方法重名。

### 4.2 宿主 `CurlEasy *`：配置（16 个，全部可入链）

| 宿主 | 方法 | 说明 | 参数 | 返回 | 链式 |
| --- | --- | --- | --- | --- | --- |
| `CurlEasy *` | `setUrl` | 设置 / 覆盖 URL | `const char *url` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setRequest` | 自定义 HTTP 方法（PUT/DELETE/HEAD…） | `const char *method` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setUserAgent` | 设置 User-Agent | `const char *ua` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setUserPwd` | Basic 认证，`"user:password"` | `const char *userpwd` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setFollow` | 跟随重定向，非 0 为开 | `int on` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setMaxRedirs` | 最大重定向次数 | `int n` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setTimeout` | 整次传输总超时，**秒** | `int seconds` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setConnectTimeout` | 连接阶段超时，**秒** | `int seconds` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setVerifyPeer` | 是否校验对端证书 | `int on` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setVerifyHost` | 主机名校验级别 | `int level` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setCainfo` | CA 证书包路径 | `const char *path` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `addHeader` | 追加一条请求头；可重复调用 | `const char *name, const char *value` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setPostFields` | 设置 POST body，**复制**，调用方无需保活 | `const char *data, int len` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setBodyLimit` | 默认 sink 的响应体上限，0 = 不限 | `int maxBytes` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setWriteFunction` | 自定义写回调；传 0 恢复默认 sink | `CurlWriteCb cb, void *ud` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `setHeaderFunction` | 自定义头回调 | `CurlHeaderCb cb, void *ud` | `CurlEasy *` | ✓ |

### 4.3 宿主 `CurlEasy *`：执行（7 个）

| 宿主 | 方法 | 说明 | 参数 | 返回 | 链式 |
| --- | --- | --- | --- | --- | --- |
| `CurlEasy *` | `perform` | 执行一次传输 | — | `CurlEasy *` | ✓ |
| `CurlEasy *` | `get` | 等价 `setRequest("GET") + perform` | — | `CurlEasy *` | ✓ |
| `CurlEasy *` | `post` | 等价 `setPostFields + perform` | `const char *data, int len` | `CurlEasy *` | ✓ |
| `CurlEasy *` | `reset` | 复位所有选项，保留连接缓存 | — | `CurlEasy *` | ✓ |
| `CurlEasy *` | `clearBody` | 清空已收到的响应体 | — | `CurlEasy *` | ✓ |
| `CurlEasy *` | `close` | 幂等释放 C 句柄；包装壳仍可读 | — | `void` | — |
| `CurlEasy *` | `destroy` | 释放整个包装壳，**只能调用一次** | — | `void` | — |

### 4.4 宿主 `CurlEasy *`：读取（10 个，全部终止）

| 宿主 | 方法 | 说明 | 参数 | 返回 |
| --- | --- | --- | --- | --- |
| `CurlEasy *` | `status` | 最近一次 `CURLcode`；`0` 是成功 | — | `int` |
| `CurlEasy *` | `message` | 该错误码的英文描述 | — | `const char *` |
| `CurlEasy *` | `errorText` | `CURLOPT_ERRORBUFFER` 的详细文本 | — | `const char *` |
| `CurlEasy *` | `responseCode` | HTTP 状态码；未收到响应为 0 | — | `int` |
| `CurlEasy *` | `effectiveUrl` | 最终 URL（跟随重定向之后） | — | `const char *` |
| `CurlEasy *` | `contentType` | 响应 Content-Type | — | `const char *` |
| `CurlEasy *` | `totalTime` | 整次传输耗时，秒 | — | `double` |
| `CurlEasy *` | `sizeDownload` | 下载字节数 | — | `long long` |
| `CurlEasy *` | `body` | 默认 sink 收到的响应体（NUL 结尾） | — | `const char *` |
| `CurlEasy *` | `bodyLen` | 响应体字节数 | — | `int` |

所有读取方法都安全接受 0 句柄：`status` 返回 `CURLE_FAILED_INIT`，字符串返回 `""`，
数字返回 0。

### 4.5 宿主 `namespace curl`：trait `Lib`（4 个）

调用写法是 `curl::Lib.<方法>`（命名空间映射必须带 trait 名，和 `sqlite::Lib.open` 一致）。

| 宿主 | 方法 | 说明 | 参数 | 返回 |
| --- | --- | --- | --- | --- |
| `namespace curl` | `globalInit` | 进程级初始化；可重复调用 | — | `int` |
| `namespace curl` | `globalCleanup` | 进程级清理 | — | `void` |
| `namespace curl` | `version` | libcurl 版本串 | — | `const char *` |
| `namespace curl` | `strerror` | 错误码对应的英文描述 | `int code` | `const char *` |

## 5. 教程正文

### 5.1 第一个请求

`openCurl()` 是 `char *` 上唯一的入口。它分配包装壳、`curl_easy_init()`、装上
错误缓冲、默认写回调与 `CURLOPT_NOSIGNAL`，然后把 URL 交出去：

```ct
CurlEasy *c = "https://example.com".openCurl();
```

返回 0 表示初始化失败（内存或 libcurl 句柄）。此后每一个配置方法都是
“写一个 option，然后把同一个指针还给你”，所以可以直接接下去。

### 5.2 链式到底是什么意思

链式不是 builder，也**不产生中间对象**。`setTimeout` 的返回值就是接收者本身，
CT 把它整条展开成一条嵌套 C 调用：

```c
ct_curl_perform(ct_curl_set_follow(ct_curl_set_timeout(
    ct_curl_open(("https://example.com")), (15)), (1)))
```

一个堆对象从头流到尾：没有临时值、没有中间句柄、没有悬垂指针。这也解释了两条写法约束：

- **不要把链拆成多行语句去“检查每一步”**——配置方法不报错，错误汇总在 `perform()` 之后；
- **链的中间结果可以接住**，接住之后仍然是同一个对象，继续链不会出问题：

```ct
CurlEasy *c = "https://example.com".openCurl().setTimeout(15);
c.setFollow(1).perform();          /* 和接着链完全等价 */
```

### 5.3 读结果

`perform()` 返回句柄本身，所以既可以在链尾直接读，也可以接住再读：

```ct
CurlEasy *c = "https://example.com".openCurl().setTimeout(15).perform();

if (c.status() != 0) {                       /* 传输层失败 */
    printf("%d %s\n", c.status(), c.message());
    printf("%s\n", c.errorText());           /* 更详细的文本 */
} else {
    printf("HTTP %d\n", c.responseCode());   /* 应用层状态 */
    printf("%s\n", c.body());
}
```

`status()` 是 `CURLcode`：连接被拒是 7，超时是 28，证书错误是 60。
`responseCode()` 是 HTTP 状态码，404 不是 CURL 错误，`status()` 仍然是 0。

### 5.4 请求头与 POST

请求头由包装壳内部的一条 slist 持有，`addHeader` 逐条追加，`close` 时统一释放：

```ct
CurlEasy *c = "https://api.example.com/items"
                  .openCurl()
                  .addHeader("Content-Type", "application/json")
                  .addHeader("Authorization", "Bearer <token>")
                  .setPostFields("{\"name\":\"x\"}", 14)
                  .perform();
```

`setPostFields` 会**复制**数据，调用方不必保活缓冲区；显式长度让 body 里可以出现 NUL。
libcurl 自己不会复制 `CURLOPT_POSTFIELDS`，所以这份副本一直留到下一次
`setPostFields` 或 `close`。

### 5.5 复用句柄

`reset()` 清掉所有 option，但保留连接缓存，适合把同一个对象反复用：

```ct
CurlEasy *c = "https://example.com/a".openCurl().setTimeout(10);

c.perform();
printf("%s\n", c.body());

c.reset().setUrl("https://example.com/b").clearBody().perform();
printf("%s\n", c.body());
```

`reset()` 会一并释放内部 header slist 与 POST 副本，并把默认 sink 重新装回去。

### 5.6 回调

回调只能写成顶层函数指针，状态通过 `void *userdata` 传：

```ct
size_t count_bytes(void *ptr, size_t size, size_t nmemb, void *ud)
{
    int *n = (int *)ud;
    (void)ptr;
    *n = *n + (int)(size * nmemb);
    return size * nmemb;
}

int total = 0;
CurlEasy *c = url.openCurl().setWriteFunction(count_bytes, &total).perform();
printf("%d bytes\n", total);
```

这才是 libcurl 的原生形态：没有闭包、没有绑定方法。`setWriteFunction` 装了自定义
回调之后，默认 sink 就不再收数据，`bodyLen()` 会保持 0；传 `0` 可以换回默认 sink。

### 5.7 收尾

```ct
c.close();      /* 幂等：释放 C 句柄，包装壳仍可读 */
c.destroy();    /* 释放包装壳；只能调用一次 */
```

只需要释放一次就结束的场合，`perform().destroy()` 是一条完整语句。
进程结束前调用 `curl::Lib.globalCleanup()`。

## 6. 常见配方

### 6.1 只看状态，不要 body

```ct
c = url.openCurl().setRequest("HEAD").setTimeout(5).perform();
printf("%d\n", c.responseCode());
```

### 6.2 限制下载体积

```ct
c = url.openCurl().setBodyLimit(8192).perform();
if (c.status() == CURLE_WRITE_ERROR) {
    printf("body over the limit; got %d bytes\n", c.bodyLen());
}
```

超限时默认 sink 返回 0，libcurl 以 `CURLE_WRITE_ERROR`（23）中止传输，
而不是悄悄截断。

### 6.3 临时关掉证书校验（仅调试）

```ct
c = url.openCurl().setVerifyPeer(0).setVerifyHost(0).perform();
```

### 6.4 自签 CA

```ct
c = url.openCurl().setCainfo("/etc/ssl/my-ca.pem").perform();
```

## 7. 排错

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 编译报 `use of an undeclared variable` 指向 `pub type` | 用的 ct 没有类型别名 | 换带 alias 的 0.20.3 或工作树构建（§2.3） |
| 链接报 `undefined reference to curl_easy_*` | 没带 `-lcurl` | `ct build ... --ldflags '-lcurl'` |
| 找不到 `curl/curl.h` | Homebrew 前缀记错 | 用系统 SDK 头，或加 `-I/opt/homebrew/opt/curl/include` |
| `status()` 是 7 | 连不上 | 检查地址、端口、防火墙 |
| `status()` 是 28 | 超时 | 调大 `setTimeout` / `setConnectTimeout` |
| `status()` 是 60 | 证书校验失败 | 查 `setCainfo`，或临时 `setVerifyPeer(0)` 定位 |
| `status()` 是 23 | 写回调返回了 0 | 检查 `setBodyLimit` 或自定义回调的返回值 |
| `status()` 是 2 | 句柄已 `close()`，或某个 setopt 失败 | 在 `close()` 之后不要再 `perform()` |
| 设置都没生效 | 忽略了返回的接收者 | `perform()` 之前不要丢掉链的结果 |
| `body()` 只有半截 | 上一次的 body 被追加 | 复用时先 `clearBody()` |

`status()` 非 0 时先看 `message()`，再看 `errorText()`：后者是 libcurl 填进
`CURLOPT_ERRORBUFFER` 的详细文本，通常比错误码有用。

## 8. 术语表（Glossary）

术语按三类收录：本包特有概念、全仓易混概念、正文出现过但未解释的英文术语。
类型名、方法名、文件名与源码逐字一致。

| 术语 | 含义 | 本包中的所指 |
| --- | --- | --- |
| easy handle | libcurl 中承载一次同步传输的句柄。 | `curl_easy_init()` 返回的 `CURL *`，被包在 `CurlEasy` 里；本包不提供 `multi` 异步句柄。 |
| `CurlEasy` | 本包唯一的对象类型，包装 easy handle 及其附属缓冲。 | `pub type CurlEasy = struct ct_curl_easy;`，结构体定义在 `curl/Client.shim.h`（`h`、`headers`、`body`、`post_copy`、`errbuf` 等字段）。 |
| `openCurl` | 从 URL 创建 `CurlEasy *` 的构造入口。 | 挂在 `char *` 上的唯一方法（trait `CurlEntry`），转调 `ct_curl_open()`；失败返回 0。 |
| 句柄（handle） | 不透明指针，调用方只通过方法操作它。 | `CurlEasy *`；C 侧 `struct ct_curl_easy` 不出现在 CT 表面。 |
| 宿主（host） | trait 方法挂靠的类型。 | 三个宿主：`char *`（`CurlEntry`）、`CurlEasy *`（`Easy`）、`namespace curl`（`Lib`）。 |
| 映射（map trait） | 把 trait 方法绑定到宿主并转调 C 函数的声明。 | `Client.m.ct` 的三段 `map trait … for …`，每行转调一个 `ct_curl_*` 函数。 |
| 链式返回自身（return receiver） | 方法返回接收者本身，使下一次调用可以接在后面。 | `CurlEasy *` 的 21 个配置 / 执行方法都 `return c;`，加上链头 `openCurl` 共 22 个；`close` / `destroy` 返回 `void`。 |
| 所有权（ownership） | 谁负责释放的约定。 | 包装壳由调用方拥有：`openCurl()` 创建，必须由 `destroy()` 释放；`close()` 只释放内部 C 句柄。 |
| 借用（borrow） | 只取得指针使用权，不负责释放。 | `body()`、`errorText()`、`effectiveUrl()`、`contentType()` 返回的 `const char *` 由包装壳或 libcurl 持有，下一次 `perform()` 或 `destroy()` 之后不保证有效。 |
| `slist` | libcurl 的 `struct curl_slist`，单向字符串链表，用来携带请求头。 | 包装壳的 `headers` 字段；`addHeader()` 用 `curl_slist_append()` 追加，`close()` 中先 `curl_easy_cleanup()` 再 `curl_slist_free_all()`，让 slist 活过 easy handle。 |
| 复制语义（copy semantics） | 被调用方是否复制调用方传入的数据。 | `addHeader()` 的行由 `curl_slist_append()` 复制；`setPostFields()` 自己 `malloc` 一份副本；其余字符串 option 由 libcurl 复制。 |
| `addHeader` | 追加一条请求头。 | `ct_curl_add_header()` 先用 `snprintf` 拼成 `"name: value"`，再追加到 slist；可重复调用。 |
| `perform` | 执行一次同步传输。 | `ct_curl_perform()` 调 `curl_easy_perform()`，把返回的 `CURLcode` 存进 `last_code`，返回接收者。 |
| `status` | 读取最近一次传输的结果码。 | `status()` 只返回 `last_code`，不发起传输；`0` 表示成功。 |
| `CURLcode` / `CURLE_*` | libcurl 的错误码枚举与常量前缀。 | `status()` 的返回值类型；`CURLE_FAILED_INIT`（2）、`CURLE_WRITE_ERROR`（23）等常量来自 `<curl/curl.h>`。 |
| `responseCode` | HTTP 状态码，属于应用层结果。 | `responseCode()` 读 `CURLINFO_RESPONSE_CODE`；404 时它非 0，而 `status()` 仍是 0。 |
| `setTimeout` / `setConnectTimeout` | 整次传输超时与连接阶段超时。 | `CURLOPT_TIMEOUT` / `CURLOPT_CONNECTTIMEOUT`，单位都是**秒**（参数名 `int seconds`），毫秒精度不在第一期。 |
| `setFollow` / `setMaxRedirs` | 是否跟随重定向，以及允许跟随的层数上限。 | `CURLOPT_FOLLOWLOCATION` / `CURLOPT_MAXREDIRS`；`setMaxRedirs` 只在 `setFollow(1)` 时生效。本包不设默认值，libcurl 8.3.0 起默认 30 层，更早版本默认不限。 |
| `setBodyLimit` | 默认 sink 允许收集的响应体字节上限。 | 包装壳的 `body_limit` 字段；`0` 表示不限，负数按 0 处理；超限时默认 sink 返回 0，传输以 `CURLE_WRITE_ERROR` 中止，而不是悄悄截断。 |
| sink | 接收响应体字节的目的地。 | 默认 sink 是 `ct_curl_default_write()`，把数据追加到包装壳的 `body`；`setWriteFunction()` 可替换它，传 0 恢复默认。 |
| 回调（callback） | 由 libcurl 在传输过程中调用的函数。 | `CurlWriteCb` / `CurlHeaderCb` 函数指针，只能写成顶层函数；没有闭包，状态通过 `userdata` 传。 |
| `userdata` | 回调的上下文指针，原样传回给回调。 | `setWriteFunction(cb, ud)` / `setHeaderFunction(cb, ud)` 的 `ud`；它归调用方所有，只需活到 `perform()` 返回。 |
| `close` | 释放 libcurl 句柄，保留包装壳。 | `ct_curl_close()` 调 `curl_easy_cleanup()`，随后释放 slist 与 POST 副本；它先检查 `closed` 标志再返回，所以可重复调用。 |
| `destroy` | 释放包装壳本身。 | `ct_curl_destroy()` 先 `close()`，再 `free(body)` 与 `free(c)`；第二次调用会访问已释放内存，所以只能调用一次。 |
| sticky `init_error` | 一次失败后保持置位的内部错误标志。 | 包装壳的 `init_error`；任一 setopt 或内存分配失败后置 1，下一次 `perform()` 直接报 `CURLE_FAILED_INIT`。 |
| `reset` | 复位所有 option 与包装壳缓冲，保留连接缓存。 | `ct_curl_reset()` 调 `curl_easy_reset()`，并释放 slist、POST 副本，清空 body，重新装回默认 sink。 |
| `CURLOPT_*` / `CURLINFO_*` | libcurl 的设置项与查询项常量前缀。 | 只出现在 shim 的 `curl_easy_setopt()` / `curl_easy_getinfo()` 调用里，CT 侧看不到。 |
| `globalInit` / `globalCleanup` | libcurl 的进程级初始化与清理。 | `namespace curl` 上的 `curl::Lib.globalInit()` / `globalCleanup()`；包装壳之外唯一要求成对调用的东西。 |
| libcurl 子系统（`multi` / `share` / `mime` / `form` / `urlapi` / `mprintf` / `WebSocket`） | libcurl 提供的异步、共享、MIME、表单、URL 解析、格式化与 WebSocket 接口。 | 本包一概不提供，也没有公开的 `CurlSlist`，理由见 §1 与 §9。 |
| hoist | 编译器把函数调用结果先落到变量，以便继续在它上面调方法。 | 本包不依赖它：`at(...).openCurl().get()` 会被拒绝，规避方法是先把 URL 接进变量（见 §9）。 |
| TLS / CA | 传输层安全协议，以及签发根证书的机构。 | `setVerifyPeer` / `setVerifyHost` / `setCainfo(path)` 都作用于 TLS 握手；`CA` 指 `CURLOPT_CAINFO` 指向的证书包。 |
| `NUL` | 字节值 0 的写法，不是字符 `'0'`。 | `setPostFields()` 的显式 `len` 让 body 可以含 NUL；`body()` 的返回值以 NUL 结尾。 |

## 9. 限制与未支持

- **`destroy()` 只能调用一次**。`close()` 是幂等的；`destroy()` 会释放包装壳本身，
  重复调用属于未定义行为。
- **函数调用结果不能当“多段链”的起点**。`at(...).openCurl()` 合法，
  但 `at(...).openCurl().get()` 会被编译器拒绝，报
  `a method call on a temporary is only allowed at the start of a statement or of an assignment`。
  这不是本包的限制，而是“函数调用结果作接收者需要 hoist”的规则只允许一层。
  规避方法：先把 URL 接进变量。
- **配置错误不就地暴露**。链式下 setter 不返回错误码，setopt 失败会置 sticky 的
  `init_error`，下一次 `perform()` 报 `CURLE_FAILED_INIT`。
- **没有闭包回调**。只有顶层函数指针 + `void *userdata`。
- **没有 `multi` / `share` / `mime` / WebSocket / `urlapi`**（见 §1）。
- **没有公开的 `CurlSlist`**。需要复用同一组 header 或直接设
  `CURLOPT_HTTPHEADER` 的场景要等第二期。
- **`CURLOPT_POSTFIELDS` 的副本由包装壳持有**，在上传类场景里意味着 body 会在内存里
  多存一份。
- **`setVerifyHost` 的 0 值**是 libcurl 的历史行为（不校验），仅用于调试。

## 10. 版本与变更记录

### 0.1.0 — 2026-11-08

首版。

- 新增 `char *` 方法：`openCurl`（台账同步回填）。
- 新增 `CurlEasy *` 方法 33 个：配置 16、执行 7、读取 10。
- 新增 `namespace curl` 能力 4 个：`globalInit` / `globalCleanup` / `version` / `strerror`。
- 链式：21 个 `CurlEasy *` 方法返回接收者（连同链头 `openCurl` 共 22 个），
  链在 C 层展开为单条嵌套调用，无临时值。
- `requires = ct >= 0.20.3`（类型别名尚未发布）。
- 测试：`tests/unit.ct` 65 条断言、`tests/chain.ct` 22 条断言、demo 输出与
  `demo/expected.out` 比对，全部通过。
