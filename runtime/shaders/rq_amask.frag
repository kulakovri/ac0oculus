#version 450
/* rq_amask.frag -- PS2_RQ_ZPRE_AMASK (rq_video.inc rqz_amask, agent clouds3 2026-11-10, R-54): the eye pass's last draw writes alpha 0 outside the
   Quest cockpit (depth bounds below its range, colour mask A only), so the only alpha left in the feed's mark band (16/255, rq_ds.frag -DNOZ) is
   the cockpit's own coverage mark: the game's sun overlays (class 5, (Cd - Cs) FIX) wrote ring-shaped alpha there, which became false cockpit
   coverage in the next pre-pass and cut the near clouds in rings. No fragment-position read. */
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0); }
