package gmhttp

import (
	"embed"
	"io/fs"
	"net/http"
)

//go:embed web
var webFiles embed.FS

// Site serves the GM web tools at /gm/: static files only, holding no secret, compiled into the program. Every power
// is behind the API's own checks, so the page is no more than a client of them.
func Site() http.Handler {
	root, err := fs.Sub(webFiles, "web")
	if err != nil {
		panic(err) // the embedded folder is checked at compile time
	}
	files := http.StripPrefix("/gm/", http.FileServerFS(root))
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			w.Header().Set("Allow", "GET, HEAD")
			http.Error(w, "Use GET.", http.StatusMethodNotAllowed)
			return
		}
		h := w.Header()
		// The page holds a login token: nothing but this origin's own files may run, and no other site may frame it.
		h.Set("Content-Security-Policy", "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
		h.Set("X-Content-Type-Options", "nosniff")
		h.Set("X-Frame-Options", "DENY")
		h.Set("Referrer-Policy", "no-referrer")
		h.Set("Cache-Control", "no-cache")
		files.ServeHTTP(w, r)
	})
}
