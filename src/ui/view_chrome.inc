// Included inside class View (private part). The chrome around the page: the top bar (view_topbar.cpp), the player
// island (view_player.cpp), the backdrop (view_motion.cpp). Members and state of those parts go here.
void top_bar(const Rect& r);  // the logo, where you are (each step clickable), the search pill, the tools
// The system's Open dialog straight away (no menu first), then what was picked plays: the Open menu's items, Ctrl+O and
// the palette's Open actions (view_player.cpp).
void open_files(bool folder);
// ui_.now at the backdrop's last drifting frame (-1: it wasn't drifting). The lights go by the clock, not by ui_.dt,
// which counts a frame paced only by the backdrop's own 33 ms timer as 1/60 s.
double drift_at_ = -1;
