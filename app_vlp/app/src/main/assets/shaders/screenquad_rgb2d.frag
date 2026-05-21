/*
 * Copyright 2026
 */

precision mediump float;
varying vec2 v_TexCoord;
uniform sampler2D u_Texture;

void main() {
  gl_FragColor = texture2D(u_Texture, v_TexCoord);
}
