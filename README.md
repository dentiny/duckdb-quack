# The Quack Client/Server Protocol for DuckDB

The `quack` extension adds a client-server protocol to DuckDB. With this extension, DuckDB can act as both a server and a client to communicate over network. For more details, please see the announcement and the DuckDB documentation. 

## Usage Example

We have to install the extension in all involved DuckDB instances
```SQL
INSTALL quack FROM core_nightly;
```

Then, we can use one DuckDB instance a s a server like so:

```SQL
LOAD quack;
CALL quack_serve('quack:localhost', token = 'super_secret');
CREATE TABLE hello AS FROM VALUES ('world') v(s);
```

And talk to this server from another instance:

```SQL
LOAD quack;
CREATE SECRET (TYPE quack, TOKEN 'super_secret');
ATTACH 'quack:localhost' AS remote;
FROM remote.hello;
```

This should show the content of the remote table `hello` on the client-side.

We can also copy data from client to server:
```SQL
-- on client
CREATE TABLE remote.hello2 AS FROM VALUES ('world2')v(s);
```
```SQL
-- on server
FROM hello2;
```


## Development
### Managing dependencies
DuckDB extensions uses VCPKG for dependency management. Enabling VCPKG via the provided makefile target:
```shell
make setup-vcpkg
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```
Then building:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/quack/quack.duckdb_extension
```
- `duckdb` is the binary for the duckdb shell with the extension code automatically loaded.
- `unittest` is the test runner of duckdb. Again, the extension is already linked into the binary.
- `quack.duckdb_extension` is the loadable binary as it would be distributed.

### Running the tests
Different tests can be created for DuckDB extensions. The primary way of testing DuckDB extensions should be the SQL tests in `./test/sql`. These SQL tests can be run using:
```sh
make test
```

