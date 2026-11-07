var _cbHashChange = function(){
	_doReplace(location.hash == '#black');
}
var _doReplace = function(_toBlack){
	var _nodes = document.getElementsByTagName('link');
	if(_toBlack){
		for(var i = _nodes.length - 1; i >= 0; i--){
			_nodes[i].href = _nodes[i].href.replace('skin/pub/', 'skin/default/');
		}
		document.title = '【黑】' + _title;
		_isBlack = true;
		location.hash = '#black';
	} else {
		for(var i = _nodes.length - 1; i >= 0; i--){
			_nodes[i].href = _nodes[i].href.replace('skin/default/', 'skin/pub/');
		}
		document.title = _title;
		_isBlack = false;
		location.hash = '#';
	}
};
var _doSwitch = function(){
	if(location.hash == '#black'){
		location.hash = '#';
	} else {
		location.hash = '#black';
	}
};

var _title = document.title;

_cbHashChange();

window.addEventListener('hashchange', _cbHashChange);
document.addEventListener('dblclick', _doSwitch);