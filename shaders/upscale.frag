#version 330 core
uniform sampler2D uMainTex;
uniform sampler2D uPrevFrameTex;
uniform sampler2D uTopBgTex;
uniform sampler2D uTopSpriteTex;
uniform vec2 uRenderResolution;
uniform vec2 uSourceResolution;
uniform vec2 uOutputResolution;
uniform float uTime;
uniform int uFrame;
uniform vec2 uScale;
uniform float uAudioVolume;
uniform float uSpeed;
in vec2 vTexCoord;
out vec4 FragColor;



vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b) {
  float l0 = ((P - m) * l) / a;
  float L0 = m - m / a;
  float L1 = m + (1.0 - m) / a;
  float S0 = m + l0;
  float S1 = m + a * l0;
  float C2 = (a * P) / (P - S1);
  float CP = -C2 / P;

  vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
  vec3 w2 = vec3(step(m + l0, x));
  vec3 w1 = vec3(1.0 - w0 - w2);

  vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
  vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
  vec3 L = vec3(m + a * (x - m));

  return T * w0 + L * w1 + S * w2;
}

vec3 uchimura(vec3 x) {
  const float P = 1.0;  // max display brightness
  const float a = 1.0;  // contrast
  const float m = 0.22; // linear section start
  const float l = 0.4;  // linear section length
  const float c = 1.33; // black
  const float b = 0.0;  // pedestal

  return uchimura(x, P, a, m, l, c, b);
}

float uchimura(float x, float P, float a, float m, float l, float c, float b) {
  float l0 = ((P - m) * l) / a;
  float L0 = m - m / a;
  float L1 = m + (1.0 - m) / a;
  float S0 = m + l0;
  float S1 = m + a * l0;
  float C2 = (a * P) / (P - S1);
  float CP = -C2 / P;

  float w0 = 1.0 - smoothstep(0.0, m, x);
  float w2 = step(m + l0, x);
  float w1 = 1.0 - w0 - w2;

  float T = m * pow(x / m, c) + b;
  float S = P - (P - S1) * exp(CP * (x - S0));
  float L = m + a * (x - m);

  return T * w0 + L * w1 + S * w2;
}

float uchimura(float x) {
  const float P = 1.5;  // max display brightness
  const float a = 1.1;  // contrast
  const float m = 0.22; // linear section start
  const float l = 0.4;  // linear section length
  const float c = 1.4; // black
  const float b = 0.0;  // pedestal

  return uchimura(x, P, a, m, l, c, b);
}

// cornerRounding use the renderResdolution as final pixel size, so it should be called after upscaling and before any post processing that assumes the output resolution.
vec4 cornerRounding(vec2 uv, vec4 color) {
  vec2 pixelPos = uv * uRenderResolution;
  vec2 fromCorner = min(pixelPos, uRenderResolution - pixelPos);
  float radius = 10.0; // corner radius in pixels
  float alpha = smoothstep(radius, radius - 1.0, length(fromCorner));
  color.a *= alpha;
  return color;
}


//higher resolution low color grain shader, to be applied after upscaling but before corner rounding and other post processing.
// This shader applies a grain effect that simulates the look of low-color, high-resolution graphics. It uses a noise function to create a subtle grain pattern that is more visible in darker areas, enhancing the nostalgic feel of pixel art while maintaining the clarity of upscaled images.
vec4 grainEffect(vec2 uv, vec4 color) {
  float grainIntensity = 0.05; // Adjust the intensity of the grain effect
  float noise = fract(sin(dot(uv * uRenderResolution, vec2(12.9898, 78.233))) * 43758.5453);
  float grain = (noise - 0.5) * grainIntensity;
  
  // Apply the grain effect more strongly to darker areas
  float brightness = dot(color.rgb, vec3(0.299, 0.587, 0.114));
  float grainStrength = smoothstep(0.0, 0.5, brightness);
  
  color.rgb += grain * grainStrength;
  return color;
}

//Suavizador de fondo sin blur.
//Solo suaviza cuando el sprite es transparente, para evitar que el fondo se vea borroso a través de los sprites.
//recibe color, y spritecolor es solo para determinar la transparencia, no se mezcla con el color final.
vec4 backgroundSmoothing(vec4 color, vec4 spriteColor) {
  float smoothingRadius = 1.6; // Adjust the radius of the smoothing effect
  float alphaThreshold = 0.1; // Threshold to determine if a pixel is considered transparent
  
  if (spriteColor.a < alphaThreshold) {
    // Only apply smoothing if the sprite pixel is transparent
    vec4 sum = vec4(0.0);
    float count = 0.0;
    
    for (float x = -smoothingRadius; x <= smoothingRadius; x++) {
      for (float y = -smoothingRadius; y <= smoothingRadius; y++) {
        vec2 offset = vec2(x, y) / uRenderResolution;
        sum += texture(uMainTex, vTexCoord + offset);
        count += 1.0;
      }
    }
    
    color.rgb = sum.rgb / count; // Average the colors of neighboring pixels
  }
  
  return color;
}

//Detecta si topBgcolor termina encima de un sprite y muestra el sprite con pequeña trasnparencia.
//Consideraciones: Si no se cumple devuelve el color original sin cambios, si el pixel del fondo esta por encima del sprite(usando color como referencia para saber si parte del sprite esta debajo del fondo) si ese es el caso devuelve la parte del sprite que no aparece en color mezclado con cierta tranparencia para simular que el sprite esta debajo del fondo, si el pixel del fondo no esta por encima del sprite devuelve el color original sin cambios.
vec4 topLayerBlending(vec4 color, vec4 topBgColor, vec4 topSpriteColor) {
  float alphaThreshold = 0.1; // Threshold to determine if a pixel is considered transparent
  
  if (topBgColor.a > alphaThreshold && color.a > alphaThreshold) {
    // If the background pixel is above the sprite pixel, blend the sprite with some transparency
    float blendFactor = 0.5; // Adjust the blend factor for transparency
    return mix(color, topSpriteColor, blendFactor);
  }
  
  return color; // Return original color if no blending is needed
}

//pixelArt Bloom, solo aplica bloom en pixeles extrictamente blancos, no claros blancos.
vec4 pixelArtBloom(vec2 uv, vec4 color) {
  float bloomThreshold = 0.95; // Threshold to determine if a pixel is considered bright enough for bloom
  float bloomIntensity = 0.5; // Adjust the intensity of the bloom effect
  
  if (color.r > bloomThreshold && color.g > bloomThreshold && color.b > bloomThreshold) {
    vec4 sum = vec4(0.0);
    float count = 0.0;
    
    for (float x = -1.0; x <= 1.0; x++) {
      for (float y = -1.0; y <= 1.0; y++) {
        vec2 offset = vec2(x, y) / uRenderResolution;
        sum += texture(uMainTex, vTexCoord + offset);
        count += 1.0;
      }
    }
    
    vec4 bloomColor = sum / count; // Average the colors of neighboring pixels
    return mix(color, bloomColor, bloomIntensity);
  }
  
  return color; // Return original color if no bloom is needed
}










// Main shader entry point.
void main()
{
  // Runtime values exposed by the SDL2 backend for shader experiments.
  vec2 renderUv = gl_FragCoord.xy / max(uRenderResolution, vec2(1.0));
  vec2 sourcePixel = vTexCoord * uSourceResolution;
  float timePhase = sin(uTime * 6.2831853);
  float framePulse = mod(float(uFrame), 120.0) / 120.0;
  float debugMix = 0.0 * (renderUv.x + sourcePixel.y + timePhase + framePulse + uScale.x + uScale.y + uAudioVolume + uSpeed + uOutputResolution.x);

    vec4 color = texture(uMainTex, vTexCoord);
    vec4 prevColor = texture(uPrevFrameTex, vTexCoord);
    
    vec4 topBgColor = texture(uTopBgTex, vTexCoord);
    vec4 topSpriteColor = texture(uTopSpriteTex, vTexCoord);
    //color = topLayerBlending(color, topBgColor, topSpriteColor);
    color = pixelArtBloom(vTexCoord, color);
    color = backgroundSmoothing(color, topSpriteColor);
    color.r = uchimura(color.r);
    color.g = uchimura(color.g);
    color.b = uchimura(color.b);

    
    
    
    color = grainEffect(renderUv, color);
    color = cornerRounding(renderUv, color);
   
    
    FragColor = color;
}