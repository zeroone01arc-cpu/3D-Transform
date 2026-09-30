/*
 * OBS Transform 3D  -  filter untuk rotasi 3D, skew, perspektif, posisi & skala
 *
 * Cara pakai: klik kanan source -> Filters -> Effect Filters (+) -> "Transform 3D"
 *
 * Konvensi sumbu (dilihat dari penonton):
 *   X = ke kanan, Y = ke bawah, Z = masuk ke layar (menjauh dari penonton)
 *   Rotasi X (+) : sisi ATAS mendekat,  sisi bawah menjauh
 *   Rotasi Y (+) : sisi KANAN mendekat, sisi kiri menjauh
 *   Rotasi Z (+) : berputar searah jarum jam (seperti rotasi biasa)
 *   Urutan rotasi: X dulu, lalu Y, lalu Z.
 *
 * Lisensi: GPL-2.0-or-later (sama dengan OBS Studio)
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <string.h>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Filter Transform 3D: rotasi X/Y/Z, skew, perspektif, posisi, skala";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Transform 3D";
}

#define DEG2RAD 0.017453292519943295f

/* ------------------------------------------------------------------ */
/* Trigonometri sendiri -> plugin tidak butuh libm (-lm) sama sekali,   */
/* sehingga build di Linux/macOS/Windows tidak bergantung setting CMake. */
/* Akurasi ~1e-7, jauh lebih dari cukup untuk transformasi visual.      */
/* ------------------------------------------------------------------ */
#define T3D_PI 3.14159265358979f
#define T3D_TWO_PI 6.28318530717959f
#define T3D_HALF_PI 1.57079632679490f

static float t3d_absf(float x)
{
	return x < 0.0f ? -x : x;
}

static float t3d_sinf(float x)
{
	x -= T3D_TWO_PI * (float)(int)(x / T3D_TWO_PI); /* ke (-2pi, 2pi) */
	if (x > T3D_PI)
		x -= T3D_TWO_PI;
	else if (x < -T3D_PI)
		x += T3D_TWO_PI;
	if (x > T3D_HALF_PI)
		x = T3D_PI - x;
	else if (x < -T3D_HALF_PI)
		x = -T3D_PI - x;

	float x2 = x * x;
	return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f +
		    x2 * (1.0f / 362880.0f + x2 * (-1.0f / 39916800.0f))))));
}

static float t3d_cosf(float x)
{
	return t3d_sinf(x + T3D_HALF_PI);
}

static float t3d_tanf(float x)
{
	return t3d_sinf(x) / t3d_cosf(x);
}

/* wrap sudut ke (-360, 360) tanpa fmodf */
static float t3d_wrap360(float a)
{
	return a - 360.0f * (float)(int)(a / 360.0f);
}

/* ------------------------------------------------------------------ */
/* Nama setting                                                        */
/* ------------------------------------------------------------------ */
#define K_PRESET "preset"
#define K_POS_X "pos_x"
#define K_POS_Y "pos_y"
#define K_POS_Z "pos_z"
#define K_ROT_X "rot_x"
#define K_ROT_Y "rot_y"
#define K_ROT_Z "rot_z"
#define K_SKEW_X "skew_x"
#define K_SKEW_Y "skew_y"
#define K_SCALE_X "scale_x"
#define K_SCALE_Y "scale_y"
#define K_FOV "fov"
#define K_AUTO_X "auto_x"
#define K_AUTO_Y "auto_y"
#define K_AUTO_Z "auto_z"
#define K_FEATHER "feather"
#define K_PAD "pad"

static const char *const all_keys[] = {K_POS_X,  K_POS_Y,  K_POS_Z,  K_ROT_X,  K_ROT_Y,
				       K_ROT_Z,  K_SKEW_X, K_SKEW_Y, K_SCALE_X, K_SCALE_Y,
				       K_FOV,    K_AUTO_X, K_AUTO_Y, K_AUTO_Z,  K_FEATHER,
				       K_PAD,    K_PRESET};

/* ------------------------------------------------------------------ */
/* Shader (di-embed supaya plugin cukup 1 file .dll / .so)             */
/*                                                                     */
/* Cara kerja: untuk setiap pixel output, tembakkan sinar dari kamera  */
/* lalu cari titik potongnya dengan bidang gambar yang sudah diputar.  */
/* Titik itu dikonversi ke koordinat tekstur -> hasil perspektif benar.*/
/* ------------------------------------------------------------------ */
static const char *effect_src =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float2 src_size;\n"
	"uniform float2 out_size;\n"
	"uniform float3 axis_x;\n"
	"uniform float3 axis_y;\n"
	"uniform float3 plane_n;\n"
	"uniform float3 trans;\n"
	"uniform float focal;\n"
	"uniform float2 skew;\n"
	"uniform float2 pscale;\n"
	"uniform float feather;\n"
	"\n"
	"sampler_state def_sampler {\n"
	"	Filter = Linear;\n"
	"	AddressU = Clamp;\n"
	"	AddressV = Clamp;\n"
	"};\n"
	"\n"
	"struct VertData {\n"
	"	float4 pos : POSITION;\n"
	"	float2 uv : TEXCOORD0;\n"
	"};\n"
	"\n"
	"VertData VSDefault(VertData v_in)\n"
	"{\n"
	"	VertData vert_out;\n"
	"	vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);\n"
	"	vert_out.uv = v_in.uv;\n"
	"	return vert_out;\n"
	"}\n"
	"\n"
	"float4 PSTransform(VertData v_in) : TARGET\n"
	"{\n"
	"	float4 result = float4(0.0, 0.0, 0.0, 0.0);\n"
	"	float2 sp = (v_in.uv - 0.5) * out_size;\n"
	"	float3 origin = float3(0.0, 0.0, -focal);\n"
	"	float3 dirv = float3(sp.x, sp.y, focal);\n"
	"	float denom = dot(plane_n, dirv);\n"
	"	float det = 1.0 - skew.x * skew.y;\n"
	"	if (abs(denom) > 0.000001 && abs(det) > 0.0001) {\n"
	"		float t = dot(plane_n, trans - origin) / denom;\n"
	"		if (t > 0.0) {\n"
	"			float3 q = origin + t * dirv - trans;\n"
	"			float a = dot(q, axis_x);\n"
	"			float b = dot(q, axis_y);\n"
	"			float u = (a - skew.x * b) / det;\n"
	"			float v = (b - skew.y * a) / det;\n"
	"			float2 full = src_size * abs(pscale);\n"
	"			float2 hs = 0.5 * full;\n"
	"			float edge = min(hs.x - abs(u), hs.y - abs(v));\n"
	"			float alpha = saturate(edge / max(feather, 0.0001) + 0.5);\n"
	"			if (alpha > 0.0) {\n"
	"				float2 uv = float2(u, v) / (src_size * pscale) + 0.5;\n"
	"				result = image.Sample(def_sampler, uv);\n"
	"				result.a = result.a * alpha;\n"
	"			}\n"
	"		}\n"
	"	}\n"
	"	return result;\n"
	"}\n"
	"\n"
	"technique Draw\n"
	"{\n"
	"	pass\n"
	"	{\n"
	"		vertex_shader = VSDefault(v_in);\n"
	"		pixel_shader = PSTransform(v_in);\n"
	"	}\n"
	"}\n";

/* ------------------------------------------------------------------ */
/* Data filter                                                         */
/* ------------------------------------------------------------------ */
struct t3d_data {
	obs_source_t *context;
	gs_effect_t *effect;

	gs_eparam_t *p_src_size, *p_out_size, *p_axis_x, *p_axis_y, *p_plane_n;
	gs_eparam_t *p_trans, *p_focal, *p_skew, *p_pscale, *p_feather;

	float pos[3];
	float rot[3];
	float skew[2];
	float scale[2]; /* dalam persen */
	float fov;
	float feather;
	float auto_speed[3];
	float auto_angle[3];
	int pad;
};

/* ------------------------------------------------------------------ */
/* Matematika kecil                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
	float m[3][3];
} mat3;

static mat3 mat3_mul(mat3 a, mat3 b)
{
	mat3 r;
	memset(&r, 0, sizeof(r));
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			float s = 0.0f;
			for (int k = 0; k < 3; k++)
				s += a.m[i][k] * b.m[k][j];
			r.m[i][j] = s;
		}
	}
	return r;
}

static mat3 rot_x(float a)
{
	float c = t3d_cosf(a), s = t3d_sinf(a);
	mat3 r = {{{1, 0, 0}, {0, c, -s}, {0, s, c}}};
	return r;
}

static mat3 rot_y(float a)
{
	float c = t3d_cosf(a), s = t3d_sinf(a);
	mat3 r = {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}};
	return r;
}

static mat3 rot_z(float a)
{
	float c = t3d_cosf(a), s = t3d_sinf(a);
	mat3 r = {{{c, -s, 0}, {s, c, 0}, {0, 0, 1}}};
	return r;
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* skala persen -> faktor, boleh negatif (cermin), tidak boleh nol */
static float scale_factor(float percent)
{
	float mag = t3d_absf(percent);
	if (mag < 1.0f)
		mag = 1.0f;
	return (percent < 0.0f ? -mag : mag) / 100.0f;
}

/* ------------------------------------------------------------------ */
/* Preset                                                              */
/* ------------------------------------------------------------------ */
struct preset {
	const char *name;
	float px, py, pz, rx, ry, rz, kx, ky, sx, sy;
};

static const struct preset presets[] = {
	{"-- Pilih preset --", 0, 0, 0, 0, 0, 0, 0, 0, 100, 100},
	{"Normal (reset transformasi)", 0, 0, 0, 0, 0, 0, 0, 0, 100, 100},
	{"Putar Y +35 (sisi kanan mendekat)", 0, 0, 0, 0, 35, 0, 0, 0, 100, 100},
	{"Putar Y -35 (sisi kiri mendekat)", 0, 0, 0, 0, -35, 0, 0, 0, 100, 100},
	{"Jungkit X +35 (sisi atas mendekat)", 0, 0, 0, 35, 0, 0, 0, 0, 100, 100},
	{"Jungkit X -35 (sisi bawah mendekat)", 0, 0, 0, -35, 0, 0, 0, 0, 100, 100},
	{"Lantai (rebah ke belakang)", 0, 0, 0, -60, 0, 0, 0, 0, 100, 100},
	{"Berlian di lantai", 0, 0, 0, -55, 0, 45, 0, 0, 100, 100},
	{"Miring / italic (skew X 20)", 0, 0, 0, 0, 0, 0, 20, 0, 100, 100},
	{"Cermin horizontal", 0, 0, 0, 0, 0, 0, 0, 0, -100, 100},
	{"Cermin vertikal", 0, 0, 0, 0, 0, 0, 0, 0, 100, -100},
	{"Balik 180 (belakang kartu)", 0, 0, 0, 0, 180, 0, 0, 0, 100, 100},
};
#define NUM_PRESETS ((int)(sizeof(presets) / sizeof(presets[0])))

/* ------------------------------------------------------------------ */
/* Callback filter                                                     */
/* ------------------------------------------------------------------ */
static const char *t3d_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Transform 3D (Rotasi / Skew / Perspektif)";
}

static void t3d_update(void *data, obs_data_t *s)
{
	struct t3d_data *f = data;

	f->pos[0] = (float)obs_data_get_double(s, K_POS_X);
	f->pos[1] = (float)obs_data_get_double(s, K_POS_Y);
	f->pos[2] = (float)obs_data_get_double(s, K_POS_Z);
	f->rot[0] = (float)obs_data_get_double(s, K_ROT_X);
	f->rot[1] = (float)obs_data_get_double(s, K_ROT_Y);
	f->rot[2] = (float)obs_data_get_double(s, K_ROT_Z);
	f->skew[0] = (float)obs_data_get_double(s, K_SKEW_X);
	f->skew[1] = (float)obs_data_get_double(s, K_SKEW_Y);
	f->scale[0] = (float)obs_data_get_double(s, K_SCALE_X);
	f->scale[1] = (float)obs_data_get_double(s, K_SCALE_Y);
	f->fov = (float)obs_data_get_double(s, K_FOV);
	f->auto_speed[0] = (float)obs_data_get_double(s, K_AUTO_X);
	f->auto_speed[1] = (float)obs_data_get_double(s, K_AUTO_Y);
	f->auto_speed[2] = (float)obs_data_get_double(s, K_AUTO_Z);
	f->feather = (float)obs_data_get_double(s, K_FEATHER);
	f->pad = (int)obs_data_get_int(s, K_PAD);
	if (f->pad < 0)
		f->pad = 0;
}

static void *t3d_create(obs_data_t *settings, obs_source_t *source)
{
	struct t3d_data *f = bzalloc(sizeof(struct t3d_data));
	f->context = source;

	obs_enter_graphics();
	char *err = NULL;
	f->effect = gs_effect_create(effect_src, "transform3d", &err);
	if (!f->effect) {
		blog(LOG_ERROR, "[Transform 3D] gagal compile shader: %s", err ? err : "(tidak ada pesan)");
	} else {
		f->p_src_size = gs_effect_get_param_by_name(f->effect, "src_size");
		f->p_out_size = gs_effect_get_param_by_name(f->effect, "out_size");
		f->p_axis_x = gs_effect_get_param_by_name(f->effect, "axis_x");
		f->p_axis_y = gs_effect_get_param_by_name(f->effect, "axis_y");
		f->p_plane_n = gs_effect_get_param_by_name(f->effect, "plane_n");
		f->p_trans = gs_effect_get_param_by_name(f->effect, "trans");
		f->p_focal = gs_effect_get_param_by_name(f->effect, "focal");
		f->p_skew = gs_effect_get_param_by_name(f->effect, "skew");
		f->p_pscale = gs_effect_get_param_by_name(f->effect, "pscale");
		f->p_feather = gs_effect_get_param_by_name(f->effect, "feather");
	}
	bfree(err);
	obs_leave_graphics();

	t3d_update(f, settings);
	return f;
}

static void t3d_destroy(void *data)
{
	struct t3d_data *f = data;
	obs_enter_graphics();
	if (f->effect)
		gs_effect_destroy(f->effect);
	obs_leave_graphics();
	bfree(f);
}

static void t3d_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, K_PRESET, 0);
	obs_data_set_default_double(s, K_POS_X, 0.0);
	obs_data_set_default_double(s, K_POS_Y, 0.0);
	obs_data_set_default_double(s, K_POS_Z, 0.0);
	obs_data_set_default_double(s, K_ROT_X, 0.0);
	obs_data_set_default_double(s, K_ROT_Y, 0.0);
	obs_data_set_default_double(s, K_ROT_Z, 0.0);
	obs_data_set_default_double(s, K_SKEW_X, 0.0);
	obs_data_set_default_double(s, K_SKEW_Y, 0.0);
	obs_data_set_default_double(s, K_SCALE_X, 100.0);
	obs_data_set_default_double(s, K_SCALE_Y, 100.0);
	obs_data_set_default_double(s, K_FOV, 60.0);
	obs_data_set_default_double(s, K_AUTO_X, 0.0);
	obs_data_set_default_double(s, K_AUTO_Y, 0.0);
	obs_data_set_default_double(s, K_AUTO_Z, 0.0);
	obs_data_set_default_double(s, K_FEATHER, 1.0);
	obs_data_set_default_int(s, K_PAD, 0);
}

static bool preset_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *s)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);

	int idx = (int)obs_data_get_int(s, K_PRESET);
	if (idx <= 0 || idx >= NUM_PRESETS)
		return false;

	const struct preset *pr = &presets[idx];
	obs_data_set_double(s, K_POS_X, (double)pr->px);
	obs_data_set_double(s, K_POS_Y, (double)pr->py);
	obs_data_set_double(s, K_POS_Z, (double)pr->pz);
	obs_data_set_double(s, K_ROT_X, (double)pr->rx);
	obs_data_set_double(s, K_ROT_Y, (double)pr->ry);
	obs_data_set_double(s, K_ROT_Z, (double)pr->rz);
	obs_data_set_double(s, K_SKEW_X, (double)pr->kx);
	obs_data_set_double(s, K_SKEW_Y, (double)pr->ky);
	obs_data_set_double(s, K_SCALE_X, (double)pr->sx);
	obs_data_set_double(s, K_SCALE_Y, (double)pr->sy);
	obs_data_set_int(s, K_PRESET, 0);
	return true;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct t3d_data *f = data;

	obs_data_t *s = obs_source_get_settings(f->context);
	for (size_t i = 0; i < sizeof(all_keys) / sizeof(all_keys[0]); i++)
		obs_data_unset_user_value(s, all_keys[i]);
	memset(f->auto_angle, 0, sizeof(f->auto_angle));
	obs_source_update(f->context, s);
	obs_data_release(s);
	return true;
}

static obs_property_t *add_slider(obs_properties_t *g, const char *key, const char *label, double lo,
				  double hi, double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(g, key, label, lo, hi, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *t3d_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	/* Preset */
	obs_property_t *pl = obs_properties_add_list(props, K_PRESET, "Preset cepat", OBS_COMBO_TYPE_LIST,
						     OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < NUM_PRESETS; i++)
		obs_property_list_add_int(pl, presets[i].name, i);
	obs_property_set_modified_callback(pl, preset_changed);

	/* Rotasi */
	obs_properties_t *g = obs_properties_create();
	add_slider(g, K_ROT_X, "Rotasi X (jungkit)", -360, 360, 0.1, "°");
	add_slider(g, K_ROT_Y, "Rotasi Y (putar kiri/kanan)", -360, 360, 0.1, "°");
	add_slider(g, K_ROT_Z, "Rotasi Z (putar datar)", -360, 360, 0.1, "°");
	obs_properties_add_group(props, "grp_rot", "Rotasi 3D", OBS_GROUP_NORMAL, g);

	/* Skew */
	g = obs_properties_create();
	add_slider(g, K_SKEW_X, "Skew X (miring horizontal)", -75, 75, 0.1, "°");
	add_slider(g, K_SKEW_Y, "Skew Y (miring vertikal)", -75, 75, 0.1, "°");
	obs_properties_add_group(props, "grp_skew", "Skew", OBS_GROUP_NORMAL, g);

	/* Posisi */
	g = obs_properties_create();
	add_slider(g, K_POS_X, "Geser X (kanan +)", -4000, 4000, 1, " px");
	add_slider(g, K_POS_Y, "Geser Y (bawah +)", -4000, 4000, 1, " px");
	add_slider(g, K_POS_Z, "Geser Z (menjauh +)", -3000, 3000, 1, " px");
	obs_properties_add_group(props, "grp_pos", "Posisi", OBS_GROUP_NORMAL, g);

	/* Skala & perspektif */
	g = obs_properties_create();
	add_slider(g, K_SCALE_X, "Skala X (negatif = cermin)", -500, 500, 1, " %");
	add_slider(g, K_SCALE_Y, "Skala Y (negatif = cermin)", -500, 500, 1, " %");
	add_slider(g, K_FOV, "Perspektif / FOV (0 = tanpa perspektif)", 0, 150, 1, "°");
	obs_properties_add_group(props, "grp_scale", "Skala & Perspektif", OBS_GROUP_NORMAL, g);

	/* Animasi */
	g = obs_properties_create();
	add_slider(g, K_AUTO_X, "Putar otomatis X", -360, 360, 1, " °/detik");
	add_slider(g, K_AUTO_Y, "Putar otomatis Y", -360, 360, 1, " °/detik");
	add_slider(g, K_AUTO_Z, "Putar otomatis Z", -360, 360, 1, " °/detik");
	obs_properties_add_group(props, "grp_auto", "Animasi (putar terus-menerus)", OBS_GROUP_NORMAL, g);

	/* Lanjutan */
	g = obs_properties_create();
	add_slider(g, K_FEATHER, "Kehalusan tepi", 0, 20, 0.1, " px");
	obs_property_t *pp = obs_properties_add_int_slider(g, K_PAD, "Ruang ekstra di sekeliling", 0, 2000, 1);
	obs_property_int_set_suffix(pp, " px");
	obs_properties_add_text(g, "pad_info",
				"Tambah ruang ekstra jika hasil putaran terpotong di tepi. Set alignment item "
				"scene ke Center supaya posisi tidak bergeser.",
				OBS_TEXT_INFO);
	obs_properties_add_group(props, "grp_adv", "Lanjutan", OBS_GROUP_NORMAL, g);

	obs_properties_add_button(props, "reset", "Reset semua", reset_clicked);
	return props;
}

static void t3d_tick(void *data, float seconds)
{
	struct t3d_data *f = data;
	for (int i = 0; i < 3; i++) {
		if (f->auto_speed[i] != 0.0f)
			f->auto_angle[i] = t3d_wrap360(f->auto_angle[i] + f->auto_speed[i] * seconds);
	}
}

static uint32_t t3d_target_w(struct t3d_data *f)
{
	obs_source_t *t = obs_filter_get_target(f->context);
	return t ? obs_source_get_base_width(t) : 0;
}

static uint32_t t3d_target_h(struct t3d_data *f)
{
	obs_source_t *t = obs_filter_get_target(f->context);
	return t ? obs_source_get_base_height(t) : 0;
}

static uint32_t t3d_get_width(void *data)
{
	struct t3d_data *f = data;
	uint32_t w = t3d_target_w(f);
	return w ? w + 2 * (uint32_t)f->pad : 0;
}

static uint32_t t3d_get_height(void *data)
{
	struct t3d_data *f = data;
	uint32_t h = t3d_target_h(f);
	return h ? h + 2 * (uint32_t)f->pad : 0;
}

static void t3d_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	struct t3d_data *f = data;

	uint32_t sw = t3d_target_w(f);
	uint32_t sh = t3d_target_h(f);
	if (!f->effect || !sw || !sh) {
		obs_source_skip_video_filter(f->context);
		return;
	}
	uint32_t ow = sw + 2 * (uint32_t)f->pad;
	uint32_t oh = sh + 2 * (uint32_t)f->pad;

	/* Matriks rotasi R = Rz * Ry * Rx */
	float rx = (f->rot[0] + f->auto_angle[0]) * DEG2RAD;
	float ry = (f->rot[1] + f->auto_angle[1]) * DEG2RAD;
	float rz = (f->rot[2] + f->auto_angle[2]) * DEG2RAD;
	mat3 R = mat3_mul(rot_z(rz), mat3_mul(rot_y(ry), rot_x(rx)));

	/* Kolom-kolom R = arah sumbu lokal gambar di ruang 3D */
	struct vec3 axis_x, axis_y, plane_n, trans;
	vec3_set(&axis_x, R.m[0][0], R.m[1][0], R.m[2][0]);
	vec3_set(&axis_y, R.m[0][1], R.m[1][1], R.m[2][1]);
	vec3_set(&plane_n, R.m[0][2], R.m[1][2], R.m[2][2]);
	vec3_set(&trans, f->pos[0], f->pos[1], f->pos[2]);

	/* Jarak kamera dari FOV (diukur terhadap sisi terpanjang) */
	float focal;
	if (f->fov < 0.5f)
		focal = 50000.0f; /* praktis ortografik */
	else
		focal = 0.5f * (float)(ow > oh ? ow : oh) / t3d_tanf(clampf(f->fov, 1.0f, 150.0f) * 0.5f * DEG2RAD);

	struct vec2 v_src, v_out, v_skew, v_scale;
	vec2_set(&v_src, (float)sw, (float)sh);
	vec2_set(&v_out, (float)ow, (float)oh);
	vec2_set(&v_skew, t3d_tanf(clampf(f->skew[0], -75.0f, 75.0f) * DEG2RAD),
		 t3d_tanf(clampf(f->skew[1], -75.0f, 75.0f) * DEG2RAD));
	vec2_set(&v_scale, scale_factor(f->scale[0]), scale_factor(f->scale[1]));

	if (!obs_source_process_filter_begin(f->context, GS_RGBA, OBS_NO_DIRECT_RENDERING))
		return;

	gs_effect_set_vec2(f->p_src_size, &v_src);
	gs_effect_set_vec2(f->p_out_size, &v_out);
	gs_effect_set_vec3(f->p_axis_x, &axis_x);
	gs_effect_set_vec3(f->p_axis_y, &axis_y);
	gs_effect_set_vec3(f->p_plane_n, &plane_n);
	gs_effect_set_vec3(f->p_trans, &trans);
	gs_effect_set_float(f->p_focal, focal);
	gs_effect_set_vec2(f->p_skew, &v_skew);
	gs_effect_set_vec2(f->p_pscale, &v_scale);
	gs_effect_set_float(f->p_feather, f->feather);

	obs_source_process_filter_end(f->context, f->effect, ow, oh);
}

/* ------------------------------------------------------------------ */
/* Registrasi                                                          */
/* ------------------------------------------------------------------ */
static struct obs_source_info transform3d_filter = {
	.id = "transform3d_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = t3d_get_name,
	.create = t3d_create,
	.destroy = t3d_destroy,
	.update = t3d_update,
	.get_defaults = t3d_defaults,
	.get_properties = t3d_properties,
	.video_tick = t3d_tick,
	.video_render = t3d_render,
	.get_width = t3d_get_width,
	.get_height = t3d_get_height,
};

bool obs_module_load(void)
{
	obs_register_source(&transform3d_filter);
	blog(LOG_INFO, "[Transform 3D] plugin dimuat");
	return true;
}
