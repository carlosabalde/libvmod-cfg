-------------------------------------------------------------------------------
-- ERROR HANDLER
-------------------------------------------------------------------------------

varnish._error_handler = function(error)
  -- Note that when the error is in the C function we want to report the
  -- information about the caller, that's what makes sense from the point of
  -- view of the user debugging a script.
  local i = debug.getinfo(2, 'nSl')
  if i and i.what == 'C' then
    i = debug.getinfo(3, 'nSl')
  end
  if i then
    return i.source .. ':' .. i.currentline .. ': ' .. error
  else
    return error
  end
end

-------------------------------------------------------------------------------
-- VARNISH.SHARED HELPERS
-------------------------------------------------------------------------------

(function(shared)
  local function serialize(value)
    local t = type(value)

    if t == 'number' then
      return 'n:' .. value
    elseif t == 'string' then
      return 's:' .. value
    elseif t == 'boolean' then
      return 'b:' .. (value and '1' or '0')
    end

    error('Failed to serialize value (type=' .. t .. ')')
  end

  local function unserialize(value)
    local result = nil

    if string.len(value) >= 2 then
      local head = string.sub(value, 1, 2)
      local body = string.sub(value, 3)

      if head == 'n:' then
        result = tonumber(body)
      elseif head == 's:' then
        result = body
      elseif head == 'b:' then
        result = body == '1'
      end
    end

    if result ~= nil then
      return result
    else
      error('Failed to unserialize value (value=' .. value .. ')')
    end
  end

  shared._get = shared.get
  shared.get = function(key, scope)
    local svalue = shared._get(key, scope)
    if svalue ~= nil then
      return unserialize(svalue)
    else
      return nil
    end
  end

  shared._set = shared.set
  shared.set = function(key, value, scope)
    if value ~= nil then
      shared._set(key, serialize(value), scope)
    else
      shared.unset(key, scope)
    end
  end

  shared.incr = function(key, increment, scope)
    local key = key
    local increment = tonumber(increment)
    if increment == nil then
      increment = 0
    end

    return shared.eval(function()
      local value = tonumber(shared.get(key, scope))
      if value == nil then
        value = increment
      else
        value = value + increment
      end

      shared.set(key, value, scope)
      return value
    end)
  end
end)(varnish.shared)
