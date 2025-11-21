import os
from adbc_driver_flightsql import dbapi as gizmosql, DatabaseOptions
import pyarrow.csv
import sys

conn = gizmosql.connect(uri="grpc+tls://localhost:31337",
                      db_kwargs={"username": os.getenv("GIZMOSQL_USERNAME", "gizmosql_username"),
                                 "password": os.getenv("GIZMOSQL_PASSWORD", "gizmosql_password"),
                                 DatabaseOptions.TLS_SKIP_VERIFY.value: "true"  # Not needed if you use a trusted CA-signed TLS cert
                                 },
                      autocommit=True
                      )
cur =  conn.cursor() 
cur.execute("SELECT * FROM lineitem")
null = open('/dev/null', 'wb')
reader = cur.fetch_record_batch()
for batch in reader:
	#pyarrow.csv.write_csv(batch, null)
	pass


# csv 1 min duckdb, 19s pyarrow