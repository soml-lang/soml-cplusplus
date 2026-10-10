api-gateway
5432
90
8080
a: int
b: array
name: 'api-gateway'
replicas: 3
timeout: 1m30s
labels: [
	'prod'
	'eu-west'
]
labels: [
	'prod'
	'eu-west'
]
name: 'api-gateway'
replicas: 3
timeout: 1m30s
1 90 db.internal 5432
name: 'api-gateway'
replicas: 1
timeout: 1m30s
labels: [
	'prod'
	'eu-west'
]
database: {
	host: 'db.internal'
	port: 5432
}
database.port: The int 70000 is outside the range of the type, 0 to 65535 at line 1, column 18
replica: Unknown key replica, which the type has no field for. Did you mean replicas? at line 2, column 1
Expected a line break before the next entry, but found “8” at line 2, column 10
  1 | name: 'api'
> 2 | port: 80 80
    |          ^
