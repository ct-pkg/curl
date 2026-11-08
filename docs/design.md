# curl 模块设计（libcurl 映射，全 OO + 链式）

- 日期：2026-11-07
- 状态：第二版设计。第一版把配置方法设计成返回 `int`，不支持链式，已作废
- 目标包：`curl`（`ct-pkg/curl`）
- 本轮**只做设计**：不创建 `ct.pkg`、不写源码、不 commit / tag / push
- 探针包（仓外）：`/private/tmp/ct-curl-probe/`

---

## 0. 结论摘要

1. **唯一构造入口放在 `char *` 上**：`"https://example.com".openCurl()` → `CurlEasy *`。
   除此之外 `char *` 不新增任何方法。
2. **所有配置与执行方法返回接收者自身类型 `CurlEasy *`**，返回值必须就是接收者指针本身
   （shim 里 `return c;`）。这包括 `perform()` / `get()` / `post()` / `reset()` / `clearBody()`。
3. **只有读取类方法和两个终止方法不返回接收者**：`status()` / `message()` / `errorText()` /
   `responseCode()` / `effectiveUrl()` / `contentType()` / `totalTime()` / `sizeDownload()` /
   `body()` / `bodyLen()` 返回标量或 `const char *`；`close()` / `destroy()` 返回 `void`，
   所以 `...perform().destroy()` 是一条合法的完整语句。
4. **链式的 C 形态**：type-host `map` 方法没有 vtable，recipe 逐层内联，链展开成一条嵌套 C 调用：
   `"url".openCurl().setTimeout(60).setFollow(1)` →
   `probe_set_follow(probe_set_timeout(probe_open("url"), 60), 1)`。
   **不产生临时变量、不产生中间对象、不存在悬垂指针**（见 §6 的探针实测）。
5. **编译器前置条件有两层，且第二层尚未发布**：
   - 字面量接收者随 ct 0.20.0 发布，已可用；
   - `pub type` 别名是 Phase 20 改动，**只存在于 ct 工作区未提交改动**。已发布的
     v0.20.0 实测编不过，v0.20.1 标签经 `git grep` 核查也不含该特性。
   - 因此本绑定在别名合入并发布前**无法用任何发布版编译**；应急方案是在 `Client.shim.h`
     里写 `typedef struct ct_curl_easy CurlEasy;`（API 与调用点不变，见 §6.6 / D-6）。
6. **链式化后不再成立的两个旧取舍**：
   - “配置方法返回 `int`，调用方逐条检查错误”——不成立；链式下错误只能汇总到
     `perform().status()`。这换来的是入口形态统一，代价是 setter 的失败不再就地可见。
   - “第一期必须暴露 `CurlSlist` 才能设请求头”——不再是硬需求。包装壳可以自己持有一条
     内部 header slist，`addHeader(name, value)` 直接追加；`CurlSlist` 降为第二期。
7. **第一期范围收敛为 easy 句柄**：内建 header 列表、内建响应体 sink、执行与读取方法。
   `multi` / `share` / `mime` / `websocket` / `form` / `urlapi` 都不进第一期。
8. **namespace 只保留进程级能力**：`globalInit` / `globalCleanup` / `version` / `strerror`。
   没有任何配置入口。slist 构造器是否也放 namespace 是待裁决项（D-3）。

---

## 1. 目标与非目标

### 1.1 目标

第一期给出一条完整、可预测、可链式书写的 HTTP 客户端路径：

```ct
import curl::Client;

#include <stdio.h>

int main(void) {
    curl::Lib.globalInit();

    CurlEasy *c = "https://example.com/health"
                      .openCurl()
                      .setTimeout(5)
                      .setFollow(1)
                      .setUserAgent("ct-curl/0.1")
                      .addHeader("Accept", "application/json")
                      .perform();

    if (c.status() != 0) {
        printf("curl error %d: %s\n", c.status(), c.errorText());
    } else {
        printf("HTTP %d, %d bytes\n", c.responseCode(), c.bodyLen());
    }

    c.destroy();
    curl::Lib.globalCleanup();
    return 0;
}
```

覆盖：

- easy 句柄的创建、配置、执行、清理；
- URL、自定义方法、超时、重定向、TLS 校验、CA、User-Agent、Basic 认证；
- 请求头（包装壳内部 slist，`addHeader` 逐条追加）；
- POST body（调用方数据由包装壳复制，调用方无需保活）；
- 默认响应体 sink + 可选写回调 / 头回调；
- HTTP 状态码、最终 URL、Content-Type、总耗时、下载字节数；
- 幂等 `close()`、显式 `destroy()`、句柄上的最近错误状态；
- 单元测试与一个可运行的 demo。

### 1.2 非目标

- 不做异步 / 事件循环（`multi` 第二期单独设计）；
- 不做 multipart 表单（`mime` 第二期）；
- 不做 WebSocket；
- 不做连接共享（`share`）；
- 不做 URL 解析器（`urlapi`）；
- 不映射已废弃的 `curl_formadd` / `curl_formfree`；
- 不暴露裸 `CURLOPT_*` / `CURLINFO_*` 变参入口；
- 不提供闭包式回调（编译器闭包能力未发布）；
- **不为了链式而引入“流式 builder”新对象**：链上的每个方法都作用在同一个 `CurlEasy` 包装壳上。

---

## 2. 能力范围与取舍

### 2.1 与现有绑定包对照

| 包 | 形态 | 句柄 | `char *` 方法 | 可复用结论 |
| --- | --- | --- | --- | --- |
| zlib | 映射式 | `z_stream *`（直接 C 类型） | `deflate` / `inflate` | 不同宿主可同名；shim 放多语句适配 |
| png | 映射式 | `PngReader *`（包装） | `readImage` / `writeImage` | 包装壳可隐藏 C 类型、提供幂等关闭 |
| sqlite | 映射式 | `SqliteDB *` / `SqliteStmt *`（包装） | `openSqliteDb` | **薄包装结构体 + 幂等 close + 回调 typedef** 直接照搬 |
| net-posix | 映射式 | `NetSocket *`（包装） | `openSocket` / `openListener` | `char *` 入口要避开 std 的 `open` |
| hal / hal-posix | 契约 / 实现分离 | 无 C 句柄 | `read` / `write` | 纯映射包不需要 `.h.ct` 层 |

本包采用 **`.m.ct` + `.shim.h` / `.shim.c`，不新增 `.h.ct`**：`.m.ct` 不能 import `.h.ct`，而本包没有
“契约 / 实现分离”的需求；sqlite 已证明包装结构体可以在 shim 里实现所有权、幂等和错误状态。

### 2.2 第一期边界

| 能力 | 第一期 | 理由 |
| --- | --- | --- |
| easy | **进** | HTTP 客户端的最小完整路径；init/setopt/perform/getinfo/cleanup 都是单次调用 |
| 内部 header slist | **进** | 包装壳自持 `struct curl_slist *`，`addHeader` 追加，`close` 释放；链式下不需要调用方管理第二个对象 |
| `CurlSlist` 公开类型 | 第二期 | 只有需要复用同一组 header 或设置 `CURLOPT_HTTPHEADER` 原始列表时才需要 |
| multi | 不进 | 需要第二套句柄、`fdset`/`poll`/`wait` 事件循环、`CURLMsg` 消息队列；而且它是**异步状态机**，与同步链式语法天然冲突，应独立设计 |
| share | 不进 | `curl_share_setopt` 也是变参；第一期没有多句柄并发 |
| mime | 不进 | 独立 `curl_mime *` 句柄 + 多方法 + 文件读取生命周期 |
| form | **不做** | 上游自 7.56.0 起标记 deprecated（`curl/curl.h:2661`） |
| websocket | 不进 | 需要 `CURLOPT_CONNECT_ONLY=2`、帧循环、`curl_ws_meta` 字段读取 |
| urlapi | 不进 | 独立 URL 解析器，不是 HTTP 传输能力 |
| mprintf | 不做 | 格式化工具，CT 侧有 printf，映射收益为零 |

### 2.3 最低 libcurl 版本

第一期建议 **libcurl >= 7.55.0**：

- `CURLINFO_SIZE_DOWNLOAD_T` / `SIZE_UPLOAD_T` 自 7.55.0 起可用（`curl/curl.h:2943`）；
- `CURLOPT_XFERINFOFUNCTION` 自 7.32.0 起可用（`curl/curl.h:1930`）；
- `curl_easy_upkeep` 自 7.62.0 起，第二期；
- `curl_mime_*` 自 7.56.0 起，第二期的最低版本抬到 7.56.0；
- WebSocket 需要 7.86.0，第三期再抬。

`ct.pkg` 的 `requires` 只能表达 **ct 编译器版本**，不能表达 libcurl 版本；libcurl 下限写在
README 与 `#if LIBCURL_VERSION_NUM` 的编译期检查里。

### 2.4 头文件核查

以下 C 名称已在 `/opt/homebrew/opt/curl/include/curl/`（8.23.0）与系统 SDK 头（8.7.1）中核对：

| 头 | 第一期使用的符号 |
| --- | --- |
| `easy.h` | `curl_easy_init` / `curl_easy_setopt` / `curl_easy_perform` / `curl_easy_getinfo` / `curl_easy_cleanup` / `curl_easy_reset` / `curl_easy_strerror` |
| `curl.h` | `curl_global_init` / `curl_global_cleanup` / `curl_version` / `curl_slist_append` / `curl_slist_free_all` / `CURLOPT_*` / `CURLINFO_*` |
| `multi.h` / `mime.h` / `websockets.h` / `urlapi.h` | 第一期不映射 |

`/opt/homebrew/include/curl/curl.h` 在本机**不存在**；Homebrew 的 curl 在 `/opt/homebrew/opt/curl/`，
不要把 `/opt/homebrew/include` 当成 Homebrew 默认前缀。

---

## 3. 类型与宿主

```ct
namespace curl;

// .m.ct 里的别名必须是 pub；每次出现都展开成底层拼写，不发射 typedef。
pub type CurlEasy  = struct ct_curl_easy;
pub type CurlSlist = struct ct_curl_slist;   // 第二期
```

- `CurlEasy *` / `CurlSlist *` 是方法宿主；C 侧真实 tag 是包私有的 `ct_curl_easy` / `ct_curl_slist`。
- **使用者只看到 `CurlEasy`，不会看到 `struct ct_curl_easy *` 双拼写**。别名在宿主键解析之前
  展开（语言规范 §4.42），所以 `map trait Easy for type CurlEasy *` 与
  `for type struct ct_curl_easy *` 是同一个宿主，不会出现平行映射。
- 不暴露裸 `CURL *`：包装壳要保存 `last_code`、`CURL_ERROR_SIZE` 错误缓冲、内部 header
  slist、默认响应体和 POST body 副本；而且 `CURL *` 是公共拼写，会和别包撞方法名。
- 不使用 `void *` 宿主：`void *` 是全仓共享类型键，任何无类型指针都能调用这些方法。
- 不新增 `const char *` 孪生宿主：`map_type_key` 会丢弃 `const`，与 `char *` 折叠后产生歧义。
- **必须是“结构体指针”而不是“结构体值”**：链式接收者如果按值传递，编译器 hoist 出来的
  临时值是结构体副本，方法里的写入会丢失。所有返回接收者的方法一律返回指针。

---

## 4. 方法表

记号：**链式 = ✓** 表示返回 `CurlEasy *`（或 `CurlSlist *`）且返回的就是接收者本身；
**—** 表示终止或纯读取。所有中文说明后的 C 映射都写在 `Client.shim.h` 里，recipe 只调用
一个类型化 helper。

### 4.1 宿主 `char *`（trait `CurlEntry`）——唯一构造入口

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `char *` | `openCurl` | 用 URL 创建一个 easy 句柄；失败返回 0 | — | `CurlEasy *` | 链头 | `ct_curl_open(url)` | 1 |

- `openCurl` 不在 `docs/char-host-methods.md` 的 37 个既有 `char *` 方法里，无冲突。
- 不叫 `open`：已被 `std::io::Path` 占用。
- `ct_curl_open` 内部：分配包装壳 → `curl_easy_init()` → `CURLOPT_URL` → `CURLOPT_ERRORBUFFER`
  → `CURLOPT_WRITEFUNCTION`/`WRITEDATA` 指向内建 sink → `CURLOPT_NOSIGNAL=1`。
- 字面量接收者 `"https://x".openCurl()` 需要 ct >= 0.20.0（Phase 20 字面量接收者已发布）。

### 4.2 宿主 `CurlEasy *`（trait `Easy`）——配置（全部返回接收者）

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CurlEasy *` | `setUrl` | 设置请求 URL（可覆盖 `openCurl` 的 URL） | `const char *url` | `CurlEasy *` | ✓ | `CURLOPT_URL` | 1 |
| `CurlEasy *` | `setRequest` | 自定义 HTTP 方法（PUT/DELETE/HEAD…） | `const char *method` | `CurlEasy *` | ✓ | `CURLOPT_CUSTOMREQUEST` | 1 |
| `CurlEasy *` | `setUserAgent` | 设置 User-Agent | `const char *ua` | `CurlEasy *` | ✓ | `CURLOPT_USERAGENT` | 1 |
| `CurlEasy *` | `setUserPwd` | Basic 认证，`"user:password"` | `const char *userpwd` | `CurlEasy *` | ✓ | `CURLOPT_USERPWD` | 1 |
| `CurlEasy *` | `setFollow` | 是否跟随重定向，非 0 为开 | `int on` | `CurlEasy *` | ✓ | `CURLOPT_FOLLOWLOCATION` | 1 |
| `CurlEasy *` | `setMaxRedirs` | 最大重定向次数 | `int n` | `CurlEasy *` | ✓ | `CURLOPT_MAXREDIRS` | 1 |
| `CurlEasy *` | `setTimeout` | 整次传输总超时，单位**秒** | `int seconds` | `CurlEasy *` | ✓ | `CURLOPT_TIMEOUT` | 1 |
| `CurlEasy *` | `setConnectTimeout` | 连接阶段超时，单位**秒** | `int seconds` | `CurlEasy *` | ✓ | `CURLOPT_CONNECTTIMEOUT` | 1 |
| `CurlEasy *` | `setVerifyPeer` | 是否校验对端证书 | `int on` | `CurlEasy *` | ✓ | `CURLOPT_SSL_VERIFYPEER` | 1 |
| `CurlEasy *` | `setVerifyHost` | 主机名校验级别 | `int level` | `CurlEasy *` | ✓ | `CURLOPT_SSL_VERIFYHOST` | 1 |
| `CurlEasy *` | `setCainfo` | CA 证书包路径 | `const char *path` | `CurlEasy *` | ✓ | `CURLOPT_CAINFO` | 1 |
| `CurlEasy *` | `addHeader` | 追加一条请求头，可重复调用；内部 slist 由包装壳持有 | `const char *name, const char *value` | `CurlEasy *` | ✓ | `curl_slist_append` + `CURLOPT_HTTPHEADER` | 1 |
| `CurlEasy *` | `setPostFields` | 设置 POST body（**复制**，调用方无需保活） | `const char *data, int len` | `CurlEasy *` | ✓ | `CURLOPT_POSTFIELDS` + `POSTFIELDSIZE` | 1 |
| `CurlEasy *` | `setBodyLimit` | 内建 sink 的响应体上限，0 = 不限 | `int maxBytes` | `CurlEasy *` | ✓ | 包内字段 | 1 |
| `CurlEasy *` | `setWriteFunction` | 自定义写回调，替代内建 sink | `CurlWriteCb cb, void *ud` | `CurlEasy *` | ✓ | `CURLOPT_WRITEFUNCTION` + `WRITEDATA` | 1 |
| `CurlEasy *` | `setHeaderFunction` | 自定义头回调 | `CurlHeaderCb cb, void *ud` | `CurlEasy *` | ✓ | `CURLOPT_HEADERFUNCTION` + `HEADERDATA` | 1 |
| `CurlEasy *` | `setAcceptEncoding` | 声明接受的压缩编码 | `const char *enc` | `CurlEasy *` | ✓ | `CURLOPT_ACCEPT_ENCODING` | 2 |
| `CurlEasy *` | `setFailOnError` | HTTP >= 400 视为错误 | `int on` | `CurlEasy *` | ✓ | `CURLOPT_FAILONERROR` | 2 |
| `CurlEasy *` | `setNoBody` | 只取头，不取 body（HEAD） | `int on` | `CurlEasy *` | ✓ | `CURLOPT_NOBODY` | 2 |
| `CurlEasy *` | `setRange` | Range 请求 | `const char *range` | `CurlEasy *` | ✓ | `CURLOPT_RANGE` | 2 |
| `CurlEasy *` | `setResumeFrom` | 断点续传偏移 | `long long offset` | `CurlEasy *` | ✓ | `CURLOPT_RESUME_FROM_LARGE` | 2 |
| `CurlEasy *` | `setProxy` | 代理地址 | `const char *proxy` | `CurlEasy *` | ✓ | `CURLOPT_PROXY` | 2 |
| `CurlEasy *` | `setReadFunction` | 自定义读回调（上传） | `CurlReadCb cb, void *ud` | `CurlEasy *` | ✓ | `CURLOPT_READFUNCTION` + `READDATA` | 2 |
| `CurlEasy *` | `setXferInfoFunction` | 进度回调 | `CurlXferInfoCb cb, void *ud` | `CurlEasy *` | ✓ | `CURLOPT_XFERINFOFUNCTION` + `XFERINFODATA` + `NOPROGRESS=0` | 2 |
| `CurlEasy *` | `setHttpHeader` | 直接挂一整条外部 slist（调用方保活） | `CurlSlist *headers` | `CurlEasy *` | ✓ | `CURLOPT_HTTPHEADER` | 2 |

单位说明：`setTimeout` / `setConnectTimeout` 取**秒**，与 libcurl 的 `CURLOPT_TIMEOUT` /
`CURLOPT_CONNECTTIMEOUT` 一致，也匹配 `setTimeout(60)` 的写法。毫秒精度第二期加
`setTimeoutMs`（D-5）。

### 4.3 宿主 `CurlEasy *`——执行（全部返回接收者，`close`/`destroy` 除外）

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CurlEasy *` | `perform` | 执行传输；错误码用 `status()` 读 | — | `CurlEasy *` | ✓ | `curl_easy_perform` | 1 |
| `CurlEasy *` | `get` | `CURLOPT_HTTPGET` 后执行，等价于 `setRequest("GET").perform()` 的快捷方式 | — | `CurlEasy *` | ✓ | 两项 setopt + perform | 1 |
| `CurlEasy *` | `post` | 设置 body 后执行；`CURLOPT_POSTFIELDS` 本身隐含 POST | `const char *data, int len` | `CurlEasy *` | ✓ | 两项 setopt + perform | 1 |
| `CurlEasy *` | `reset` | 复位所有选项（保留已建立的连接） | — | `CurlEasy *` | ✓ | `curl_easy_reset` + 重新装 errbuf/sink | 1 |
| `CurlEasy *` | `clearBody` | 清空内建响应体缓冲，便于复用句柄 | — | `CurlEasy *` | ✓ | 包内字段 | 1 |
| `CurlEasy *` | `close` | 释放 libcurl 句柄（幂等），保留包装壳 | — | `void` | — | `curl_easy_cleanup` | 1 |
| `CurlEasy *` | `destroy` | 关闭并释放包装壳；**只能调用一次** | — | `void` | — | `close` + `free` | 1 |

`perform` 返回接收者而不是 `int` 是本版设计的核心取舍：

- 链尾可以继续接读取方法：`...perform().status()`；
- 链尾可以直接销毁：`...perform().destroy()`；
- 代价：`if (c.perform())` 不再成立，错误判断改写为 `if (c.perform().status() != 0)` 或
  `c.perform(); if (c.status() != 0)`。
- 如果 `perform` 返回 `int`，链尾就再也拿不到句柄，读取 body 必须把链拆成两条语句，
  与“全面链式”的目标冲突。推荐 A（见 D-1）。

### 4.4 宿主 `CurlEasy *`——读取（终止，不返回接收者）

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CurlEasy *` | `status` | 最近一次操作的 `CURLcode`，0 = OK | — | `int` | — | 包装壳 `last_code` | 1 |
| `CurlEasy *` | `message` | `CURLcode` 的标准文案 | — | `const char *` | — | `curl_easy_strerror` | 1 |
| `CurlEasy *` | `errorText` | 详细错误文本（ERRORBUFFER） | — | `const char *` | — | 包装壳 `errbuf` | 1 |
| `CurlEasy *` | `responseCode` | HTTP 状态码 | — | `int` | — | `CURLINFO_RESPONSE_CODE` | 1 |
| `CurlEasy *` | `effectiveUrl` | 跟随重定向后的最终 URL | — | `const char *` | — | `CURLINFO_EFFECTIVE_URL` | 1 |
| `CurlEasy *` | `contentType` | 响应 Content-Type | — | `const char *` | — | `CURLINFO_CONTENT_TYPE` | 1 |
| `CurlEasy *` | `totalTime` | 总耗时（秒，含小数） | — | `double` | — | `CURLINFO_TOTAL_TIME` | 1 |
| `CurlEasy *` | `sizeDownload` | 下载字节数 | — | `long long` | — | `CURLINFO_SIZE_DOWNLOAD_T` | 1 |
| `CurlEasy *` | `body` | 内建 sink 的内容（NUL 结尾） | — | `const char *` | — | 包装壳 `body` | 1 |
| `CurlEasy *` | `bodyLen` | 内建 sink 的字节数 | — | `int` | — | 包装壳 `body_len` | 1 |
| `CurlEasy *` | `sizeUpload` | 上传字节数 | — | `long long` | — | `CURLINFO_SIZE_UPLOAD_T` | 2 |
| `CurlEasy *` | `redirectCount` | 重定向次数 | — | `int` | — | `CURLINFO_REDIRECT_COUNT` | 2 |
| `CurlEasy *` | `httpVersion` | 协商到的 HTTP 版本 | — | `int` | — | `CURLINFO_HTTP_VERSION` | 2 |
| `CurlEasy *` | `sslVerifyResult` | TLS 校验结果 | — | `int` | — | `CURLINFO_SSL_VERIFYRESULT` | 2 |

### 4.5 宿主 `CurlSlist *`（trait `Slist`）——第二期

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `CurlSlist *` | `append` | 追加一行（如 `"Accept: application/json"`） | `const char *line` | `CurlSlist *` | ✓ | `curl_slist_append` | 2 |
| `CurlSlist *` | `free` | 释放整条列表 | — | `void` | — | `curl_slist_free_all` | 2 |
| `CurlSlist *` | `count` | 行数 | — | `int` | — | 包内字段 | 2 |
| `namespace curl` | `newHeaders` | 创建空 slist（无接收者，故留在 namespace） | — | `CurlSlist *` | 链头 | `ct_curl_slist_new()` | 2 |

如果第二期保留 slist，构造器只有这一种没有接收者的形态；若坚持“`char *` 上只保留
`openCurl`”，也可以改成 `"".openCurlHeaders()`，但不推荐（D-3）。

### 4.6 宿主 `namespace curl`（trait `Lib`）——只有进程级能力

| 宿主 | 方法 | 中文说明 | 参数 | 返回 | 链式 | C 映射 | 期 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `namespace curl` | `globalInit` | 进程级初始化，进程启动调用一次 | — | `int` | — | `curl_global_init` | 1 |
| `namespace curl` | `globalCleanup` | 进程级清理 | — | `void` | — | `curl_global_cleanup` | 1 |
| `namespace curl` | `version` | libcurl 版本字符串 | — | `const char *` | — | `curl_version` | 1 |
| `namespace curl` | `strerror` | 任意 `CURLcode` 的文案 | `int code` | `const char *` | — | 包内 helper | 1 |
| `namespace curl` | `newHeaders` | 创建空 slist | — | `CurlSlist *` | 链头 | `ct_curl_slist_new()` | 2 |
| `namespace curl` | `escape` | URL 编码 | `const char *s` | `const char *` | — | `curl_easy_escape` | 2 |
| `namespace curl` | `unescape` | URL 解码 | `const char *s` | `const char *` | — | `curl_easy_unescape` | 2 |
| `namespace curl` | `versionInfo` | 结构化版本信息 | — | `CurlVersion *` | — | `curl_version_info` | 2 |

**namespace 里没有任何 setter / 配置入口**。`escape` 在第二期实现时要处理返回缓冲的所有权
（`curl_free`），届时再定返回值形态。

### 4.7 方法数统计

| trait | 宿主 | 第一期 | 第二期 | 合计 |
| --- | --- | --- | --- | --- |
| `CurlEntry` | `char *` | 1 | 0 | 1 |
| `Easy` | `CurlEasy *` | 33 | 13 | 46 |
| `Slist` | `CurlSlist *` | 0 | 3 | 3 |
| `Lib` | `namespace curl` | 4 | 4 | 8 |
| **合计** | — | **38** | **20** | **58** |

其中返回接收者、可入链的方法：第一期 **22 个**（`openCurl` 1 + 配置 16 + 执行 5），
第二期 **10 个**（配置 9 + `Slist.append` 1）。`close` / `destroy` 返回 `void`，不计入。

---

## 5. 链式语义

### 5.1 三条规则

- **R1｜返回接收者本身**：所有标 ✓ 的方法在 shim 里必须 `return c;`，不能返回新分配的副本，
  也不能返回 `0`。链上每一步都是同一个包装壳指针。
- **R2｜recipe 里接收者只出现一次**：`map` 展开时，若 recipe 把接收者参数用了不止一次，
  实参必须是可重复求值的表达式（变量 / 字面量 / 字段链），否则编译器报
  `map recipe uses '<%s>' more than once; bind the argument to a local first`。
  因此规定：**每个 recipe 只写一次接收者**，需要多次使用时调用一个只接收一次句柄的 shim
  helper（例如 `ct_curl_add_header(c, name, value)` 内部自己动 slist）。
- **R3｜终止方法返回 `void`**：`close()` / `destroy()` 不能出现在链中间，避免
  “关闭后继续配置”的悬垂使用。它们只能出现在链尾。

### 5.2 完整示例

**标准 GET，链尾接住句柄：**

```ct
CurlEasy *c = "https://example.com"
                  .openCurl()
                  .setTimeout(10)
                  .setFollow(1)
                  .setVerifyPeer(1)
                  .addHeader("Accept", "text/html")
                  .perform();

printf("HTTP %d, %d bytes, %.3fs\n", c.responseCode(), c.bodyLen(), c.totalTime());
c.destroy();
```

**错误判断写在链尾：**

```ct
CurlEasy *c = "https://example.com/missing".openCurl().setTimeout(5).setFollow(1).perform();
if (c.status() != 0) {
    printf("curl error %d: %s\n", c.status(), c.errorText());
}
c.destroy();
```

句柄已经在变量里时，也可以把 `status()` 直接接在 `perform()` 后面：

```ct
int rc = c.clearBody().perform().status();
```

注意：不要把 `openCurl()` 开头的整条链直接接 `status()` 后丢弃返回值——那会丢掉唯一的
句柄引用，包装壳泄漏。要么接住句柄，要么用 `...perform().destroy()`。

**一条语句发完即销毁（只关心副作用）：**

```ct
"https://example.com/ping".openCurl().setTimeout(2).perform().destroy();
```

**中间结果接住，再继续链：**

```ct
CurlEasy *c = "https://example.com".openCurl().setTimeout(10);
c.setFollow(1).addHeader("X-Trace", "abc").perform();
printf("%s\n", c.body());
c.destroy();
```

**POST：**

```ct
CurlEasy *c = "https://example.com/api/items"
                  .openCurl()
                  .setTimeout(15)
                  .setRequest("POST")
                  .addHeader("Content-Type", "application/json")
                  .setPostFields("{\"name\":\"x\"}", 12)
                  .perform();
```

### 5.3 别名与所有权（链式带来的新注意点）

- `a.openCurl().setTimeout(60)` 与 `a` 指向**同一个对象**；把中间结果接住再继续链，不会复制句柄。
- 因此 **`destroy()` 只能调用一次**；两个变量别名同一句柄时，只销毁一次，另一变量随即可丢弃。
- 链尾接住句柄后必须 `destroy()`；只写 `...perform();` 而不接住结果会泄漏包装壳
  （libcurl 句柄与内部缓冲都在里面）。需要“发完即弃”时用 `...perform().destroy()`。
- `close()` 之后包装壳还在，但 libcurl 句柄已释放；再 `perform()` 返回
  `CURLE_FAILED_INIT` 一类错误，不会崩溃（shim 里检查 `closed`）。

---

## 6. 编译探针（实测记录）

### 6.1 使用的编译器

| 编译器 | 版本 | 用途 | 结论 |
| --- | --- | --- | --- |
| `/tmp/ctbuild/ct` | 自报 `ct v0.20.1` | **本轮实测使用的编译器** | 六个探针全部通过 |
| `/private/tmp/ct-v0200/ct` | ct v0.20.0 | 第一轮探针 | **不支持 `pub type`**，探针包在别名行就编译失败 |
| `/private/tmp/docbuild/ct-ast` | `ct-ast v0.20.0` | 候选 | 同样太旧，未使用 |
| `/Users/samxx/.local/bin/ct` | ct v0.19.1 | — | 太旧，字面量接收者与别名都不支持 |

关于 `/tmp/ctbuild/ct` 的构建来源（**重要**）：

- 它由 ct 工作树 `/Users/samxx/vscspace/ct` 按 L0 → L1 → L2 → ct-ast 自举链构建
  （命令见 §6.3），工作树 `git describe --tags` = `v0.20.1-1-g5ef3ed6`。
- `pub type` 别名**只存在于工作区未提交改动**（`Symbols.h.ct` / `Module.h.ct` / `Expr.h.ct` /
  `Parser.h.ct` 等 9 个文件，另有 `docs/design/phase20-type-alias.md`）。
- `git grep 'pub type' v0.20.1 -- selfhost` 无输出，说明 **v0.20.1 标签本身也不含该特性**；
  `git log v0.20.1..HEAD` 只有一条文档提交。
- 所以“ct v0.20.1 能编”的准确表述是：**“含 Phase 20 type alias 的工作区版 ct 能编”**。
  已发布版本（0.20.0 / 0.20.1）都不能编译这个绑定，直到别名改动合入并发布。

用户指定的 `/tmp/ct-0.20.0-worktree/ct` 在本机**不存在**（该路径下没有可执行文件）。

### 6.2 探针源码

探针包（仓外，可用 `sh /private/tmp/ct-curl-probe/run.sh` 复跑）：

```text
/private/tmp/ct-curl-probe/
├── ct.pkg                  # name = probe; requires = ct >= 0.20.3
├── probe/
│   ├── Curl.m.ct           # pub type CurlEasy + trait Easy / Entry + map
│   └── Curl.shim.h         # 假包装壳，所有 helper 返回同一个 struct ct_curl_easy *
├── p1_literal.ct
├── p2_result_receiver.ct
├── p3_chain3.ct
├── p4_tail_assign.ct
├── p5_mid_and_full.ct
├── p6_perform_status.ct
└── run.sh
```

最小映射单元（完整内容见 `/private/tmp/ct-curl-probe/probe/Curl.m.ct`）：

```ct
namespace probe;

pub type CurlEasy = struct ct_curl_easy;

pub trait Easy {
    CurlEasy *setTimeout(int ms);
    CurlEasy *setFollow(int on);
    CurlEasy *setHeader(const char *name, const char *value);
    CurlEasy *setPost(const char *body, int len);
    CurlEasy *setUserPwd(const char *userpwd);
    CurlEasy *perform();
    int status();
    int timeout();
    int follow();
    int close();
    void destroy();
}

pub trait Entry {
    CurlEasy *openCurl();
}

map trait Easy for type CurlEasy * {
    setTimeout(c, ms) -> probe_set_timeout(c, ms);
    setFollow(c, on) -> probe_set_follow(c, on);
    setHeader(c, name, value) -> probe_set_header(c, name, value);
    setPost(c, body, len) -> probe_set_post(c, body, len);
    setUserPwd(c, userpwd) -> probe_set_userpwd(c, userpwd);
    perform(c) -> probe_perform(c);
    status(c) -> probe_status(c);
    timeout(c) -> probe_timeout(c);
    follow(c) -> probe_follow(c);
    close(c) -> probe_close(c);
    destroy(c) -> probe_destroy(c);
}

map trait Entry for type char * {
    openCurl(url) -> probe_open(url);
}
```

六个探针分别覆盖：

| # | 文件 | 验证点 |
| --- | --- | --- |
| P1 | `p1_literal.ct` | `"https://x".openCurl()`：字面量作接收者 |
| P2 | `p2_result_receiver.ct` | `c0.setTimeout(60)`：方法调用结果赋给变量，返回值类型与接收者一致 |
| P3 | `p3_chain3.ct` | `"…".openCurl().setTimeout(60).setFollow(1).setUserPwd("u:p")`：四段连续链 |
| P4 | `p4_tail_assign.ct` | 链尾 `perform()` 的结果赋给 `CurlEasy *` 后继续读取 |
| P5 | `p5_mid_and_full.ct` | 中间结果接住再继续链；链出现在函数实参位置 |
| P6 | `p6_perform_status.ct` | `...perform().status()`：链尾返回接收者再调方法 |

### 6.3 复跑命令（本轮实测使用的命令）

```sh
# 1) 从 ct 工作树自举（工作区含 Phase 20 type alias 未提交改动）
mkdir -p /tmp/ctbuild
gcc -Wall -Wextra -std=c99 selfhost/l0/ctc.c -o /tmp/ctbuild/ctc
/tmp/ctbuild/ctc selfhost/l1/ctc.ct -o /tmp/ctbuild/ctc-l1.c
gcc -Wall -Wextra -std=c99 /tmp/ctbuild/ctc-l1.c -o /tmp/ctbuild/ctc-l1
mkdir -p /tmp/ctbuild/l2
/tmp/ctbuild/ctc-l1 build selfhost/l2/driver.ct -o /tmp/ctbuild/l2
gcc -Wall -Wextra -std=c99 /tmp/ctbuild/l2/*.c -o /tmp/ctbuild/ctc-l2
mkdir -p /tmp/ctbuild/ast
/tmp/ctbuild/ctc-l2 build -I selfhost/ct-ast selfhost/ct-ast/main.ct -o /tmp/ctbuild/ast
gcc -Wall -Wextra -std=c99 /tmp/ctbuild/ast/*.c -o /tmp/ctbuild/ct
/tmp/ctbuild/ct --version        # ct v0.20.1

# 2) 跑六个探针（工作目录任意）
CT=/tmp/ctbuild/ct sh /private/tmp/ct-curl-probe/run.sh
```

`run.sh` 对六个探针逐个 `ct build -I <pkg> --outdir <work>/.ctbuild --cflags '-Wall -Wextra'`，
然后运行，并打印生成的 C 里所有 `probe_*` 调用与 `__ct_tmp` 命中情况。

### 6.4 实测结果

完整输出保存在 `/private/tmp/ct-curl-probe/run.out`，`run.sh` 退出码 **0**。

**负面结果（v0.20.0）**：

`pub type` 在已发布的 ct v0.20.0 上不被识别，探针包在第一个别名行就失败
（`probe/Curl.m.ct:3:28`）：

```text
error: use of an undeclared variable
```

同一编译器编译仓内既有包也能复现（`sqlite/sqlite/Sqlite.m.ct:31`，正是
`pub type SqliteDB = struct ct_sqlite_db;`）：

```text
error: use of an undeclared variable
```

**正面结果（含 type alias 的工作区版，自报 ct v0.20.1）**：

| 探针 | 构建 | 运行输出 | 结论 |
| --- | --- | --- | --- |
| P1 `p1_literal` | ok | `p1: ok` | 字面量作接收者成立 |
| P2 `p2_result_receiver` | ok | `p2: timeout=60` | 方法调用结果可赋给变量，返回类型就是 `CurlEasy *` |
| P3 `p3_chain3` | ok | `p3: timeout=60 follow=1` | 四段连续链成立 |
| P4 `p4_tail_assign` | ok | `p4: status=0 timeout=60` | 链尾 `perform()` 返回句柄，可继续读取 |
| P5 `p5_mid_and_full` | ok | `p5: timeout=60 follow=1 status=0` / `p5: arg position 5` | 中间结果可接住再续链；链可出现在函数实参位置 |
| P6 `p6_perform_status` | ok | `p6: rc=0` | `...perform().status()` 成立 |

六个探针均 **build ok、无 `warning:`**（`run.sh` 对 warning 置失败）。

**生成的 C（原文摘录）**：

```c
/* p1 */
struct ct_curl_easy * c = probe_open(("https://x"));

/* p2 */
c = probe_set_timeout((c0), (60));

/* p3：四段链被内联成一条嵌套表达式 */
struct ct_curl_easy * c =
    probe_set_userpwd((probe_set_follow((probe_set_timeout((probe_open(("https://x"))), (60))), (1))), ("u:p"));

/* p4：链尾 perform */
struct ct_curl_easy * c =
    probe_perform((probe_set_follow((probe_set_timeout((probe_open(("https://x"))), (60))), (1))));

/* p5：中间结果 mid 续链；第二条在实参位置 */
struct ct_curl_easy * mid = probe_set_timeout((probe_open(("https://x"))), (60));
struct ct_curl_easy * c =
    probe_perform((probe_set_post((probe_set_header((probe_set_follow((mid), (1))), ("X-Test"), ("1"))), ("a=1"), (3))));
printf("p5: arg position %d\n",
       probe_timeout((probe_set_follow((probe_set_timeout((probe_open(("https://y"))), (5))), (0)))));

/* p6 */
int rc = probe_status((probe_perform((probe_set_follow((probe_set_timeout((probe_open(("https://x"))), (60))), (1))))));
```

`== __ct_tmp occurrences (expected: none) ==` 之后**没有任何输出**：整条链没有生成临时变量。

### 6.5 结论：链式在编译器层面成立

- **没有临时值**：type-host `map` 方法逐层内联，链就是一条嵌套 C 调用，`__ct_tmpN` 一次都没出现。
- **没有悬垂指针**：每个 helper 返回的是同一个 `struct ct_curl_easy *`（shim 里 `return c;`），
  链上的每一步都指向同一个堆对象；对象生命周期由 `openCurl` / `destroy` 管理，与链的长度无关。
- **不需要降级写法**：P1–P6 覆盖了字面量接收者、方法结果作接收者、三段以上链、链尾赋值、
  中间变量续链、实参位置、链尾再调方法，全部成立。
- **两条实现约束来自本次实测的 C 形态**：
  1. 每个 recipe 里接收者只能出现一次（否则 `map_repeatable` 会对调用型实参报
     `map recipe uses '%s' more than once; bind the argument to a local first`）；
  2. 返回接收者的方法必须返回**指针**。若返回结构体值，链上每层都会是值拷贝，
     配置写入会丢失（C 形态里每层都是 `struct ct_curl_easy *`，这一点已经验证）。

### 6.6 降级方案（保留，作为编译器回归时的应急）

本轮探针全部通过，下面的方案不启用；若未来编译器回归或需要支持没有别名的版本，按序降级：

1. **目标编译器没有 `pub type`**：在 `Client.shim.h` 里写
   ```c
   typedef struct ct_curl_easy CurlEasy;
   ```
   CT 侧仍写 `CurlEasy *`，API 与调用点完全不变；代价是生成 C 里出现一个真实 typedef，
   而不是“纯编译期同义词”。宿主键要统一写成 `CurlEasy *`，不要混用 `struct ct_curl_easy *`。
2. **方法结果不能作接收者**：阶段性用变量接住每一步；API 不变，只限制书写：
   ```ct
   CurlEasy *c = "https://x".openCurl();
   c.setTimeout(60);
   c.setFollow(1);
   c.perform();
   ```
3. **需要改编译器时的最小改动**：
   - 若解析期对 `N_MCALL` 接收者取不到类型：在 `parse_postfix` 生成内层 `N_MCALL` 时确保把
     方法声明的返回类型写入该节点的 `text`，再解析外层接收者；
   - 若发射期对 `N_MCALL` 接收者走了 vtable 分支：在 `Emitter.c` 的接收者种类检查里补上
     `N_MCALL`，并按“函数调用结果”的规则 hoist 成 `__ct_tmpN`（类型是 `CurlEasy *`，安全）；
   - 这两处都不改变 C 侧 ABI，只影响绑定写法。

### 6.7 实现期补测：函数调用结果不能当“多段链”的起点

落地 `tests/unit.ct` 时补测了一条 §6.4 没覆盖的组合，结论是**不成立**，已写入 README §8：

```ct
c = at("/hello", buf).openCurl();          /* 成立：函数调用结果作接收者，一层 */
c = at("/hello", buf).openCurl().get();    /* 不成立 */
```

报错原文（`ct build`，工作树版 ct）：

```text
unit.ct:235:1: error: a method call on a temporary is only allowed at the start of a statement or of an assignment
```

原因：函数调用结果作接收者需要 hoist 成 `__ct_tmpN`，而 hoist 只允许出现在语句开头或赋值
右侧；一旦这层 hoist 出来，外层方法调用的接收者又变成“临时值”，第二次 hoist 不被允许。
注意**字面量接收者不受影响**（`"https://x".openCurl().setTimeout(60)` 照常成立），
变量接收者也不受影响——这只针对“函数调用 + 两段以上”的组合。

降级写法：先把 URL 接进变量，再起链。`tests/chain.ct` 的 22 条用例按这个前提编写，
其中“字面量接收者”和“方法结果作接收者”仍是独立用例。

---

## 7. 回调与 userdata

CT 侧可用的回调形态只有一种：**顶层函数指针 + `void *userdata`**。没有闭包、没有绑定方法、
没有类型化 userdata、没有泛型。sqlite 的 `exec_cb` 已验证这条路径
（`Sqlite.m.ct:81` 用 shim 里的 `ct_sqlite_cb` typedef 作参数类型）。

回调类型在 `Client.shim.h` 里用 C typedef 声明，CT 侧只引用 typedef 名
（与 sqlite 的做法一致，不在 CT 里手写函数指针类型）：

```c
typedef size_t (*CurlWriteCb)(void *ptr, size_t size, size_t nmemb, void *userdata);
typedef size_t (*CurlHeaderCb)(void *ptr, size_t size, size_t nmemb, void *userdata);
typedef size_t (*CurlReadCb)(void *ptr, size_t size, size_t nmemb, void *userdata);
typedef int (*CurlXferInfoCb)(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                              curl_off_t ultotal, curl_off_t ulnow);
```

- 第一期默认不装用户写回调，包装壳内建 sink 负责收集 body；`setWriteFunction` 可覆盖。
- 用户回调必须声明为顶层函数，返回类型与 typedef 一致（写/读/头回调返回 `size_t`），
  否则 `-Wall -Wextra` 会报函数指针类型不匹配。
- userdata 原样透传，CT 侧只能传结构体 / 数组指针；回调里自行转换。
- **不做**“回调绑定到某个 `CurlEasy` 实例”的自动化：那需要 trampoline + 类型化 userdata，
  属于第二期。

---

## 8. 生命周期与所有权

```c
struct ct_curl_easy {
    CURL *h;
    int last_code;                 /* 最近一次 CURLcode */
    int closed;
    char errbuf[CURL_ERROR_SIZE];  /* CURLOPT_ERRORBUFFER，始终有效 */
    struct curl_slist *headers;    /* 包装壳自持，close 时释放 */
    char *post_copy;               /* CURLOPT_POSTFIELDS 的副本 */
    int post_len;
    char *body;                    /* 默认响应体 sink */
    int body_len;
    int body_cap;
    int body_limit;
    int body_overflow;
    CurlWriteCb write_cb;
    void *write_ud;
    CurlHeaderCb header_cb;
    void *header_ud;
};
```

- `openCurl()` 失败返回 0；成功返回的包装壳由调用方拥有。
- `close()` 幂等：释放 `h`、`headers`、`post_copy`，保留 `body` / `errbuf` 供读取。
- `destroy()` **只能调用一次**：先 `close()`，再释放 `body` 与包装壳。`free` 之后的对象
  无法再证明幂等，所以可重复调用的是 `close()`，不是 `destroy()`。
- `setPostFields` 复制数据；`addHeader` 复制进 slist；`setUrl` 等不复制（libcurl 自己复制）。
- 内部 slist 的生命周期覆盖 `h`：`close()` 里先 `curl_easy_cleanup` 再
  `curl_slist_free_all`，顺序不能反。
- `reset()` 后重新安装 errbuf 与默认 sink；`body` 清空，`last_code` 归零。
- **链式不改变所有权**：链上的每一步都返回同一个指针，所有权始终在最初接住它的人手里。

---

## 9. 错误模型

- `CURLcode` 用 `int` 表示，`0 = CURLE_OK`。
- **所有 setter 都不中断链**。`setopt` 失败会写 `last_code` 并置 sticky `init_error`；
  后续 `perform()` 若发现 `init_error`，直接返回 `CURLE_FAILED_INIT` 而不发请求。
- 读取顺序建议：`status()` → `errorText()`（更详细）→ `message()`（标准文案）。
- `perform()` 返回接收者后，错误判断有两种等价写法：
  ```ct
  CurlEasy *c = "…".openCurl().setTimeout(5).perform();
  if (c.status() != 0) { … }
  ```
  ```ct
  int rc = "…".openCurl().setTimeout(5).perform().status();
  ```
- HTTP 4xx/5xx **不是** `CURLcode` 错误（除非 `setFailOnError(1)`）；用 `responseCode()` 判断。

---

## 10. 编译器前置条件

| 能力 | 可用版本 | 依据 |
| --- | --- | --- |
| 字面量接收者 | ct 0.20.0 起（已发布） | 语言规范记录；探针 P1 通过 |
| `type` 别名（`pub type X = struct tag;`） | **未发布**，只在 ct 工作区改动里 | 语言规范 §4.42；v0.20.0 实测编不过；`git grep 'pub type' v0.20.1 -- selfhost` 无输出 |
| type-host map | 已发布 | zlib / sqlite 既有用法 |
| `map trait Entry for type char *` | 已发布 | zlib 的 `char *` 宿主 |
| 闭包 | **不可用** | roadmap Phase 21，目标 v0.21.0，未发布 |

结论：

- 绑定本身**不需要**新的编译器能力；它只需要“别名 + 字面量接收者”，其中别名已经实现
  （工作区版），只差合入与发布。
- `requires` 定为 **`ct >= 0.20.3`**：已发布的 0.20.2（`0263cca`）不含类型别名，别名将随
  0.20.3 发布；README 与 `ct.pkg` 都如实标注“0.20.3 尚未发布，编译验证使用含 alias 的
  工作树构建”。不能写 0.20.0 / 0.20.2（实测编不过）。
- 若必须在现有发布版上落地，改用 §6.6 方案 1 的 shim typedef，删除 `pub type` 两行，
  其余设计不变（D-6）。

---

## 11. 跨平台与依赖

- 包默认链接系统 `-lcurl`，不硬编码 Homebrew 路径；`ldflags = -lcurl` 写进 `ct.pkg`。
- macOS：系统 curl 8.7.1 可用；Homebrew curl 8.23.0 可用，但用户需同时把 `-I` 与 `-L`
  指向同一套头/库（`/opt/homebrew/opt/curl/`），不能只换头。
- Linux：`libcurl4-openssl-dev` / `libcurl-devel`。
- TLS 后端差异（SecureTransport / OpenSSL / GnuTLS）不进入 public API。
- `curl_off_t` 在 macOS/Linux LP64 是 `long`，Windows 是 `long long`；`sizeDownload()` 用
  `long long` 承接 `CURLINFO_SIZE_DOWNLOAD_T`，避免平台差异。
- 数值型 `CURLOPT_*` 一律按 `(long)` 传；`CURLOPT_RESUME_FROM_LARGE` 按 `(curl_off_t)` 传。

---

## 12. 目录结构与测试

```text
curl/
├── ct.pkg                  # requires = ct >= 0.20.3; ldflags = -lcurl
├── LICENSE
├── .gitignore
├── curl/
│   ├── Client.m.ct         # namespace curl; traits CurlEntry / Easy / Lib
│   ├── Client.shim.h       # 包装壳、回调 typedef、helper 声明
│   └── Client.shim.c       # helper 唯一实现（非 static inline）
├── tests/
│   ├── unit.ct             # 65 条断言，覆盖全部 38 个方法
│   ├── chain.ct            # 22 条链式回归断言
│   ├── run.sh
│   └── fixture_server.c
├── demo/
│   ├── main.ct
│   └── expected.out
├── README.md
└── docs/
    └── design.md
```

实现阶段把 shim 拆成 `.shim.h` + `.shim.c` 两半（设计稿原先只写了头文件）：
helper 是普通外部函数，一个程序只有一份实现，也方便 `cc -Wall -Wextra` 单独检查；
回调 typedef 仍留在头文件里，CT 侧只引用 typedef 名。

不用 `Curl.m.ct` / `Curl.shim.h` 命名：macOS 大小写不敏感，生成的 `Curl.h` 可能遮蔽
`<curl/curl.h>`。

测试要点：

- 单元测试用一个本地 fixture server（`fork` + socket），覆盖 GET/POST/重定向/超时/404/
  大 body/header 回显/Basic 认证/body 上限；
- 链式专项 `tests/chain.ct`：字面量接收者、方法结果再作接收者、四段链、链尾赋值、
  `perform().status()`、`perform().destroy()`、中间结果接住后续链、接收者指针恒等；
- `run.sh` 用 `-Wall -Wextra`，任何 warning 视为失败（与 zlib / sqlite 一致）；
- demo 输出归一化 URL 行后与 `demo/expected.out` 做 golden diff。

---

## 13. 仍需裁决的事项

### 13.1 旧 10 条在链式下的去留

| # | 旧问题 | 链式后是否还成立 | 建议 |
| --- | --- | --- | --- |
| 1 | 第一期是否包含 `multi` | 仍不进；且理由更强：`multi` 是异步状态机，链式是同步单请求语法，两者不该共用一套方法 | 不进 |
| 2 | 默认响应体策略 | 仍成立，且更重要：链式里没有地方写分支，默认 sink 让链尾直接可读 | 保留默认 sink |
| 3 | 回调暴露程度 | 仍成立；`setWriteFunction` 返回接收者，可入链 | 双轨 |
| 4 | POST body 所有权 | 仍成立；链式下临时 body 更常见，借用必然悬垂 | 复制 |
| 5 | 句柄形态 | 仍成立；链式要求返回接收者，只有包装壳能承载 `last_code`/`errbuf`/body | 包装壳 |
| 6 | 是否暴露裸 `setopt` 变参 | 仍不暴露；变参无法与“返回接收者”的类型签名统一 | 不暴露 |
| 7 | libcurl 最低版本 | 不变 | 7.55.0 |
| 8 | 包布局 | 不变 | 单 `.m.ct` + `.shim.h` / `.shim.c` |
| 9 | `escape`/`unescape`/`versionInfo` | 仍第二期；`version` 第一期 | 按表 |
| 10 | 链接系统 curl 还是 Homebrew | 不变 | 系统 `-lcurl` |

### 13.2 新增待裁决

| # | 问题 | 选项 | 推荐 |
| --- | --- | --- | --- |
| D-1 | `perform()` 返回什么 | A. `CurlEasy *`（链尾可继续 `status()`/`destroy()`）；B. `int`（就地判错，但断链） | **A** |
| D-2 | `close()` / `destroy()` 返回什么 | A. `void`（终止语义）；B. `CurlEasy *`（可以继续接，但容易误用） | **A** |
| D-3 | `CurlSlist` 构造器放哪 | A. `curl::Lib.newHeaders()`（真正无接收者）；B. `"".openCurlHeaders()`（`char *` 上多一个入口） | **A**；且 slist 整体降第二期 |
| D-4 | 第一期是否完全不做公开 `CurlSlist` | A. 做（内建 header 已够用）；B. 不做 | **A** |
| D-5 | 超时单位 | A. `setTimeout(seconds)`；B. `setTimeoutMs(ms)`；C. 两个都提供 | **A**（第二期再加 ms） |
| D-6 | 别名未发布，`requires` 与落地方式怎么定 | A. 等 Phase 20 alias 合入并随 0.20.3 发布，`requires = ct >= 0.20.3` 并在 README 注明未发布；B. 先用 shim `typedef` 兼容 0.20.2；C. 写 0.20.0 / 0.20.2（实测编不过，不可接受） | **A**（已裁决并落地） |
| D-7 | 链尾不接住时是否提供 `performAndForget()` | A. 不提供，用 `...perform().destroy()`；B. 提供 | **A** |
| D-8 | `addHeader` 与 `setHttpHeader(slist)` 是否并存 | A. 第一期只 `addHeader`；B. 两者都第一期 | **A** |

---

## 14. 实现状态

设计已落地（2026-11-08）：

1. 探针 P1–P6 全部通过（§6.4）；实现期补测的“函数调用结果 + 多段链”限制见 §6.7。
2. D-1～D-8 按推荐项裁决：`perform()` 与全部配置方法返回 `CurlEasy *`，`close` / `destroy`
   返回 `void`，第一期不做公开 `CurlSlist`。
3. 已交付：`ct.pkg`、`curl/Client.m.ct`、`curl/Client.shim.h`、`curl/Client.shim.c`、
   `tests/`（`unit.ct` + `chain.ct` + `fixture_server.c` + `run.sh`）、`demo/`、README。
4. 验证命令与结果：`CT=<带 alias 的 ct> bash tests/run.sh` → harness 8 项全过；
   `unit.ct` 65 条断言、`chain.ct` 22 条断言、demo 与 `expected.out` 一致、
   38/38 个公开方法名被调用；编译全程 `-Wall -Wextra` 零告警。
5. 台账 `docs/char-host-methods.md` 已回填 `openCurl`（`char *` 37 → 38，全仓 215 → 216）。

按约定未做：不 commit、不打 tag、不动 `index/` 与其他包。

---

## 15. 实现与设计的差异

| # | 设计稿原写法 | 落地写法 | 原因 |
| --- | --- | --- | --- |
| 1 | 只有 `Client.shim.h` | 拆成 `.shim.h` + `.shim.c` | helper 作为普通外部函数只保留一份实现；头文件只放结构体、回调 typedef 与声明 |
| 2 | 目录 `test/` | 目录 `tests/` | 按交付要求命名 |
| 3 | `destroy()` 幂等 | `destroy()` 只能调用一次；`close()` 幂等 | 包装壳被 `free` 之后无法再证明幂等，文档如实区分 |
| 4 | 示例写 `curl::globalInit()` | `curl::Lib.globalInit()` | 命名空间映射的调用必须带 trait 名（与 `sqlite::Lib.open` 一致） |
| 5 | 未讨论“函数调用结果 + 多段链” | 明确不支持，写入 §6.7 与 README §8 | 编译器 hoist 规则只允许一层 |
| 6 | demo 只写了一个 `get.ct` | `demo/main.ct` + `demo/expected.out` | 让 demo 成为可校验产物：`run.sh` 用夹具跑它并做 golden diff |
| 7 | `ct.pkg` `requires = ct >= 0.20.1` | `ct >= 0.20.3` | 0.20.2（`0263cca`）实测不含别名，别名随 0.20.3 发布 |

除上表外，方法表、宿主划分、返回值与链式语义均按设计稿实现，无额外差异。
