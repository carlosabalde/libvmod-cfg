/******************************************************************************
** VARNISH.SHARED HELPERS
******************************************************************************/

(function(shared) {
  function serialize(value) {
    var t = typeof value;

    if (t == 'number') {
      return 'n:' + value;
    } else if (t == 'string') {
      return 's:' + value;
    } else if (t == 'boolean') {
      return 'b:' + (value ? '1' : '0');
    }

    throw 'Failed to serialize value (type=' + t + ')';
  }

  function unserialize(value) {
    var result = null;

    if (value.length >= 2) {
      var head = value.substring(0, 2);
      var body = value.substring(2);

      if (head == 'n:') {
        result = Number(body);
      } else if (head == 's:') {
        result = body;
      } else if (head == 'b:') {
        result = body == '1';
      }
    }

    if (result !== null) {
      return result;
    } else {
      throw 'Failed to unserialize value (value=' + value + ')';
    }
  }

  shared._get = shared.get;
  shared.get = function(key, scope) {
    var svalue = shared._get(key, scope);
    if (svalue !== null) {
      return unserialize(svalue);
    } else {
      return null;
    }
  };

  shared._set = shared.set;
  shared.set = function(key, value, scope) {
    if (value !== null) {
      shared._set(key, serialize(value), scope);
    } else {
      shared.unset(key, scope);
    }
  };

  shared.incr = function(key, increment, scope) {
    var key = key;
    var increment = parseInt(increment)
    if (isNaN(increment)) {
      increment = 0;
    }

    return shared.eval(function() {
      var value = parseInt(shared.get(key, scope));
      if (isNaN(value)) {
        value = increment;
      } else {
        value += increment;
      }

      shared.set(key, value, scope);
      return value;
    })
  };
})(varnish.shared);
