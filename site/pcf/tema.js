// Tema común de pcfsite.com (cookie «tema»: light, dark o system; la comparten todos los *.pcfsite.com). Antes de pintar.
(function(){var d=document.documentElement,m=document.cookie.match(/(?:^|; )tema=(light|dark|system)/),t=m?m[1]:null;
t=t||"system";
function ap(){if(t==="light"||t==="dark"){d.dataset.theme=t}else{if(window.matchMedia&&matchMedia("(prefers-color-scheme: dark)").matches)d.dataset.theme="dark";else delete d.dataset.theme}}
ap();window.pcfTema=function(n){t=n;ap()};window.pcfTemaActual=function(){return t};
if(window.matchMedia){var q=matchMedia("(prefers-color-scheme: dark)");(q.addEventListener?q.addEventListener("change",function(){if(t==="system")ap()}):0)}})();
