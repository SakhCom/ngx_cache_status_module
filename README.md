# cache_status

Модуль собирает статистику по кэшированию запросов и предоставляет
доступ к этой статистике.

## Сборка

```
./configure ..... --add-module=PATH_TO_MODULE/ngx_cache_status_module
make
```

## Директивы

```
Синтаксис: cache_status [prom];
Значение по умолчанию: ---
Контекст: location
```
Предоставляет статистику кэширования.

## Пример конфигурации

```
location /cache-stat {
    cache_status;
}

location /cache-stat-prom {
    cache_status prom;
}
```

## Пример данных

```
Cache statistics:
Requests: 15
Uncached: 10
Miss: 2
Bypass: 0
Expired: 1
Stale: 0
Updating: 0
Revalidated: 0
Hit: 2
Misc: 0
```

```
# HELP nginx_cache_status nginx cache status
# TYPE nginx_cache_status counter
nginx_cache_status{status="total"} 17
nginx_cache_status{status="uncached"} 10
nginx_cache_status{status="miss"} 0
nginx_cache_status{status="bypass"} 0
nginx_cache_status{status="expired"} 1
nginx_cache_status{status="stale"} 0
nginx_cache_status{status="updating"} 0
nginx_cache_status{status="revalidated"} 0
nginx_cache_status{status="hit"} 6
nginx_cache_status{status="misc"} 0
```