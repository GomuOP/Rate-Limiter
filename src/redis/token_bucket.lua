-- Atomic token-bucket check (design 0.6, 0.7).
--
-- Runs as a single EVALSHA so the read-modify-write cannot interleave between
-- proxy instances. Deterministic: the current time is passed in as ARGV[1]
-- rather than read via redis.call('TIME'), so the script is replication- and
-- AOF-safe.
--
-- KEYS[1]  bucket hash key
-- ARGV[1]  now_ms   (client wall-clock, milliseconds since epoch)
-- ARGV[2]  rate     (tokens added per second)
-- ARGV[3]  burst    (bucket capacity)
-- ARGV[4]  cost     (tokens this request wants)
--
-- returns { allowed(0|1), tokens_remaining, retry_after_ms, reset_ms }

local key   = KEYS[1]
local now   = tonumber(ARGV[1])
local rate  = tonumber(ARGV[2])
local burst = tonumber(ARGV[3])
local cost  = tonumber(ARGV[4])

local state  = redis.call('HMGET', key, 'tokens', 'ts')
local tokens = tonumber(state[1])
local ts     = tonumber(state[2])
if tokens == nil then
    tokens = burst
    ts = now
end

local delta = now - ts
if delta < 0 then
    delta = 0
end
tokens = math.min(burst, tokens + (delta / 1000.0) * rate)

local allowed = 0
if tokens >= cost then
    tokens = tokens - cost
    allowed = 1
end

local retry_after = 0
if allowed == 0 then
    retry_after = math.ceil(((cost - tokens) / rate) * 1000.0)
end
local reset = math.ceil(((burst - tokens) / rate) * 1000.0)

redis.call('HSET', key, 'tokens', tokens, 'ts', now)
redis.call('PEXPIRE', key, reset + 1000)

return { allowed, math.floor(tokens), retry_after, reset }
