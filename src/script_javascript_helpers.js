/******************************************************************************
** VARNISH.SHARED HELPERS
******************************************************************************/

varnish.shared.incr = function(key, increment, scope) {
  var key = key;
  var increment = parseInt(increment)
  if (isNaN(increment)) {
    increment = 0;
  }

  return varnish.shared.eval(function() {
    var value = parseInt(varnish.shared.get(key, scope));
    if (isNaN(value)) {
      value = increment;
    } else {
      value += increment;
    }

    varnish.shared.set(key, value, scope);
    return value;
  })
};
