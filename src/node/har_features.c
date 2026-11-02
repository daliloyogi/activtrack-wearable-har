/*
 * har_features.c -- the 30-feature window transform, ported from
 * feature_extractor.py / train_ebm.py. Statistics are population (ddof=0)
 * to match numpy's defaults.
 */
#include <math.h>
#include "har_ebm.h"

#define EPS 1e-6

const char *const har_labels[HAR_N_CLASSES] = {
    "Standing", "Sitting", "Walking", "Running"};

const char *const har_feature_names[HAR_N_FEATURES] = {
    "accel_std", "accel_mean", "accel_max", "accel_min", "accel_range",
    "accel_rms", "accel_energy",
    "gyro_std", "gyro_mean", "gyro_max", "gyro_min", "gyro_range",
    "gyro_rms", "gyro_energy",
    "accelX_mean", "accelY_mean", "accelZ_mean",
    "accelX_std", "accelY_std", "accelZ_std",
    "gyroX_mean", "gyroY_mean", "gyroZ_mean",
    "gyroX_std", "gyroY_std", "gyroZ_std",
    "standing_indicator", "axis_gap_1", "axis_gap_2", "axis_gap_ratio"};

/* Population mean and standard deviation of one axis. */
static void axis_stats(const float *v, har_acc_t *mean_out, har_acc_t *std_out)
{
    har_acc_t sum = 0.0, mean, var = 0.0;
    int i;

    for (i = 0; i < HAR_WINDOW_SIZE; ++i)
        sum += (har_acc_t)v[i];
    mean = sum / HAR_WINDOW_SIZE;

    for (i = 0; i < HAR_WINDOW_SIZE; ++i)
    {
        har_acc_t d = (har_acc_t)v[i] - mean;
        var += d * d;
    }

    *mean_out = mean;
    *std_out = (har_acc_t)sqrt((double)(var / HAR_WINDOW_SIZE));
}

/*
 * The seven magnitude statistics shared by the accelerometer and gyroscope
 * blocks: std, mean, max, min, range, rms, energy -- written to out[0..6].
 */
static void magnitude_stats(const float *x, const float *y, const float *z,
                            float *out)
{
    har_acc_t mag[HAR_WINDOW_SIZE];
    har_acc_t sum = 0.0, sum_sq = 0.0, mean, var = 0.0;
    har_acc_t max_v, min_v;
    int i;

    for (i = 0; i < HAR_WINDOW_SIZE; ++i)
    {
        har_acc_t xi = (har_acc_t)x[i];
        har_acc_t yi = (har_acc_t)y[i];
        har_acc_t zi = (har_acc_t)z[i];
        mag[i] = (har_acc_t)sqrt((double)(xi * xi + yi * yi + zi * zi));
        sum += mag[i];
        sum_sq += mag[i] * mag[i];
    }

    mean = sum / HAR_WINDOW_SIZE;
    max_v = min_v = mag[0];
    for (i = 0; i < HAR_WINDOW_SIZE; ++i)
    {
        har_acc_t d = mag[i] - mean;
        var += d * d;
        if (mag[i] > max_v)
            max_v = mag[i];
        if (mag[i] < min_v)
            min_v = mag[i];
    }

    out[0] = (float)sqrt((double)(var / HAR_WINDOW_SIZE));    /* std    */
    out[1] = (float)mean;                                     /* mean   */
    out[2] = (float)max_v;                                    /* max    */
    out[3] = (float)min_v;                                    /* min    */
    out[4] = (float)(max_v - min_v);                          /* range  */
    out[5] = (float)sqrt((double)(sum_sq / HAR_WINDOW_SIZE)); /* rms    */
    out[6] = (float)sum_sq;                                   /* energy */
}

void har_extract_features(const har_window_t *w, float out[HAR_N_FEATURES])
{
    har_acc_t ax_mean, ay_mean, az_mean, ax_std, ay_std, az_std;
    har_acc_t gx_mean, gy_mean, gz_mean, gx_std, gy_std, gz_std;
    har_acc_t sorted[3], tmp;
    har_acc_t gap1, gap2;

    magnitude_stats(w->accel_x, w->accel_y, w->accel_z, &out[HAR_F_ACCEL_STD]);
    magnitude_stats(w->gyro_x, w->gyro_y, w->gyro_z, &out[HAR_F_GYRO_STD]);

    axis_stats(w->accel_x, &ax_mean, &ax_std);
    axis_stats(w->accel_y, &ay_mean, &ay_std);
    axis_stats(w->accel_z, &az_mean, &az_std);
    axis_stats(w->gyro_x, &gx_mean, &gx_std);
    axis_stats(w->gyro_y, &gy_mean, &gy_std);
    axis_stats(w->gyro_z, &gz_mean, &gz_std);

    out[HAR_F_ACCELX_MEAN] = (float)ax_mean;
    out[HAR_F_ACCELY_MEAN] = (float)ay_mean;
    out[HAR_F_ACCELZ_MEAN] = (float)az_mean;
    out[HAR_F_ACCELX_STD] = (float)ax_std;
    out[HAR_F_ACCELY_STD] = (float)ay_std;
    out[HAR_F_ACCELZ_STD] = (float)az_std;
    out[HAR_F_GYROX_MEAN] = (float)gx_mean;
    out[HAR_F_GYROY_MEAN] = (float)gy_mean;
    out[HAR_F_GYROZ_MEAN] = (float)gz_mean;
    out[HAR_F_GYROX_STD] = (float)gx_std;
    out[HAR_F_GYROY_STD] = (float)gy_std;
    out[HAR_F_GYROZ_STD] = (float)gz_std;

    /* Orientation block: the three axis means sorted ascending. */
    sorted[0] = ax_mean;
    sorted[1] = ay_mean;
    sorted[2] = az_mean;
    if (sorted[0] > sorted[1])
    {
        tmp = sorted[0];
        sorted[0] = sorted[1];
        sorted[1] = tmp;
    }
    if (sorted[1] > sorted[2])
    {
        tmp = sorted[1];
        sorted[1] = sorted[2];
        sorted[2] = tmp;
    }
    if (sorted[0] > sorted[1])
    {
        tmp = sorted[0];
        sorted[0] = sorted[1];
        sorted[1] = tmp;
    }

    gap1 = (har_acc_t)fabs((double)(sorted[1] - sorted[0]));
    gap2 = (har_acc_t)fabs((double)(sorted[2] - sorted[1]));

    out[HAR_F_STANDING_INDICATOR] =
        (float)(fabs((double)(ax_mean - az_mean)) /
                (fabs((double)(ay_mean - ax_mean)) + EPS));
    out[HAR_F_AXIS_GAP_1] = (float)gap1;
    out[HAR_F_AXIS_GAP_2] = (float)gap2;
    out[HAR_F_AXIS_GAP_RATIO] = (float)(gap2 / (gap1 + EPS));
}

int har_ebm_argmax(const float scores[HAR_N_CLASSES])
{
    int best = 0, i;
    for (i = 1; i < HAR_N_CLASSES; ++i)
        if (scores[i] > scores[best])
            best = i;
    return best;
}

int har_classify(const har_window_t *w)
{
    float features[HAR_N_FEATURES];
    float scores[HAR_N_CLASSES];

    har_extract_features(w, features);
    har_ebm_predict(features, scores);
    return har_ebm_argmax(scores);
}

#ifdef HAR_WANT_PROBABILITIES
void har_ebm_softmax(const float scores[HAR_N_CLASSES],
                     float probabilities[HAR_N_CLASSES])
{
    float max_s = scores[har_ebm_argmax(scores)];
    float sum = 0.0f;
    int i;

    for (i = 0; i < HAR_N_CLASSES; ++i)
    {
        probabilities[i] = expf(scores[i] - max_s);
        sum += probabilities[i];
    }
    for (i = 0; i < HAR_N_CLASSES; ++i)
        probabilities[i] /= sum;
}
#endif
