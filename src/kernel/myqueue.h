
// 定义队列节点
struct MyQueueNode
{
    struct proc *data;
    struct MyQueueNode *next;
};

// 定义队列结构
struct MyQueue
{
    struct MyQueueNode *front; // 队头
    struct MyQueueNode *rear;  // 队尾
};

// 初始化队列
void initQueue(struct MyQueue *q)
{
    q->front = q->rear = NULL;
}

// 入队操作
void enqueue(struct MyQueue *q, struct proc *value)
{
    struct MyQueueNode *newNode = (struct MyQueueNode *)kalloc(sizeof(struct MyQueueNode));

    newNode->data = value;
    newNode->next = NULL;

    if (q->rear == NULL)
    {
        // 队列为空
        q->front = q->rear = newNode;
    }
    else
    {
        q->rear->next = newNode;
        q->rear = newNode;
    }
}

// 出队操作
struct proc *dequeue(struct MyQueue *q)
{

    struct proc *value = q->front->data;
    struct MyQueueNode *temp = q->front;

    q->front = q->front->next;
    if (q->front == NULL)
    {
        q->rear = NULL;
    }

    kfree(temp);
    return value;
}

// 队列是否为空
int isEmpty(struct MyQueue *q)
{
    return q->front == NULL;
}

// 清空队列
void clearQueue(struct MyQueue *q)
{
    while (!isEmpty(q))
    {
        dequeue(q);
    }
}

// 销毁队列
void destroyQueue(struct MyQueue *q)
{
    clearQueue(q);
}