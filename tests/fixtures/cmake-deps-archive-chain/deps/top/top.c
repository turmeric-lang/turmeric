int mid_value(void);
int core_value(void);
int top_value(void) { return mid_value() + core_value(); }
