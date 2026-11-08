// ProShadeLite.fx - ajustari controlate de addon-ul ProShadeLite (Brightness, Brighter Nights, Lighter Shadows, Sky Color)
// Pune fisierul in reshade-shaders\Shaders si activeaza tehnica ProShadeLite (addonul o porneste/opreste singur).
#include "ReShade.fxh"

uniform float PSL_Brightness     < ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_label = "Brightness"; > = 0.0;
uniform float PSL_BrighterNights < ui_type = "slider"; ui_min =  0.0; ui_max = 1.0; ui_label = "Brighter Nights"; > = 0.0;
uniform float PSL_LighterShadows < ui_type = "slider"; ui_min =  0.0; ui_max = 1.0; ui_label = "Lighter Shadows"; > = 0.0;
uniform float PSL_SkyAmount      < ui_type = "slider"; ui_min =  0.0; ui_max = 1.0; ui_label = "Sky Color amount"; > = 0.0;
uniform float3 PSL_SkyColor      < ui_type = "color"; ui_label = "Sky Color"; > = float3(0.35, 0.55, 1.0);

float3 PSL_Pass(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float3 c = tex2D(ReShade::BackBuffer, uv).rgb;
	float luma = dot(c, float3(0.2126, 0.7152, 0.0722));

	// Cer: bandele intunecate din imagine sunt ridicate (shadows / nights)
	float dark = 1.0 - smoothstep(0.0, 0.35, luma);
	c += c * dark * PSL_LighterShadows * 1.5;
	c += dark * PSL_BrighterNights * 0.12 * (1.0 - luma);

	// Brightness global
	c *= exp2(PSL_Brightness);

	// Sky Color: cerul = adancime la maxim (necesita depth buffer corect in joc)
	float depth = ReShade::GetLinearizedDepth(uv);
	float sky = step(0.999, depth);
	c = lerp(c, PSL_SkyColor * (0.4 + luma), sky * PSL_SkyAmount);

	return saturate(c);
}

technique ProShadeLite
{
	pass { VertexShader = PostProcessVS; PixelShader = PSL_Pass; }
}
