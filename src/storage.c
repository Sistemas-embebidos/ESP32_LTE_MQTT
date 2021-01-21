#include "storage.h"

data_t datas;

extern int threshold_min;
extern int threshold_max;
extern int rate;

extern QueueHandle_t Queue_data,Queue_config,Queue_state;

esp_err_t save_read_data(data_t datas)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read
    int32_t data_saved_counter = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "saved_counter", &data_saved_counter);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;

    // Write
    data_saved_counter++;
    err = nvs_set_i32(my_handle, "saved_counter", data_saved_counter);
    if (err != ESP_OK) return err;

    char aux[STRING_LENGTH_BIG];
    char idx[7];

    sprintf(idx,"i_%d",data_saved_counter % MAX_BUFFER_RING);
    sprintf(aux,DATA_FORMAT,datas.timestamp,datas.temperature_1,datas.temperature_2,datas.temperature_3,3.3*(float)(datas.battery)/0xFFF);
    ESP_LOGI(NVS_TAG,"Saving <%s> %s",idx,aux);

    nvs_set_str(my_handle,idx,aux);

    // Commit written value.
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t load_config(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Read threshold_min
    //int32_t threshold_min = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "threshold_min", &threshold_min);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("threshold_min = %d\n", threshold_min);

    // Read threshold_max
    //int32_t threshold_max = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "threshold_max", &threshold_max);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("threshold_max = %d\n", threshold_max);

    // Read rate
    //int32_t rate = 0; // value will default to 0, if not set yet in NVS
    err = nvs_get_i32(my_handle, "rate", &rate);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    printf("rate = %d\n", rate);

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

esp_err_t save_config(void)
{
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "threshold_min", threshold_min);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "threshold_max", threshold_max);
    if (err != ESP_OK) return err;

    // Write
    err = nvs_set_i32(my_handle, "rate", rate);
    if (err != ESP_OK) return err;

    // Commit written value.
    err = nvs_commit(my_handle);
    if (err != ESP_OK) return err;

    // Close
    nvs_close(my_handle);
    return ESP_OK;
}

int parserJsonIntValues( char const* json, int* parsedValues )
{
    bool saveNextChar = false;
    char stringValue[10];
    int stringValueIndex = 0;
    int i = 0;
    int numberOfParsedValues = 0;

    while( json[i] != '\0' ) {
        //printf( "i: %d\r\n", i );
        if( json[i] == ':' ) {
            saveNextChar = true;
            i++;
            continue;
        }
        if( json[i] == ',' || json[i] == '}' ) {
            stringValue[stringValueIndex] = '\0';
            parsedValues[numberOfParsedValues] = atoi(stringValue); // ASCII to Integer
            numberOfParsedValues++;
            stringValueIndex = 0;
            saveNextChar = false;
            if( json[i] == '}' ) {
                return numberOfParsedValues;
            }
            i++;
            continue;
        }
        if( saveNextChar ) {
            stringValue[stringValueIndex] = json[i];
            stringValueIndex++;
        }
        i++;
    }
    return numberOfParsedValues;
}
