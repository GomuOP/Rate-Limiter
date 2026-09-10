-- wrk script: hit /check with a spread of keys so the limiter actually works.
--
--   wrk -t4 -c100 -d30s -s bench/check.lua http://127.0.0.1:8080
--
-- Env:
--   RL_KEYS   number of distinct keys to cycle through (default 1000)

local nkeys = tonumber(os.getenv("RL_KEYS") or "1000")
local counter = 0

request = function()
    counter = counter + 1
    local key = "k" .. (counter % nkeys)
    return wrk.format("GET", "/check?key=" .. key)
end

local status = {}
response = function(code)
    status[code] = (status[code] or 0) + 1
end

done = function(summary, latency, requests)
    io.write("\n--- status codes ---\n")
    for code, n in pairs(status) do
        io.write(string.format("  %s : %d\n", tostring(code), n))
    end
    io.write(string.format("latency p50=%.2fms p90=%.2fms p99=%.2fms max=%.2fms\n",
        latency:percentile(50) / 1000,
        latency:percentile(90) / 1000,
        latency:percentile(99) / 1000,
        latency.max / 1000))
end
