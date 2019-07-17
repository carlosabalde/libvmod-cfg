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

varnish.shared.incr = function(key, increment, scope)
  local key = key
  local increment = tonumber(increment)
  if increment == nil then
    increment = 0
  end

  return varnish.shared.eval(function()
    local value = tonumber(varnish.shared.get(key, scope))
    if value == nil then
      value = increment
    else
      value = value + increment
    end

    varnish.shared.set(key, value, scope)
    return value
  end)
end
