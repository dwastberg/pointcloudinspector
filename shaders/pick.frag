#version 450

layout(location = 0) flat in uint pointId;
layout(location = 0) out uint outputId;

void main()
{
    outputId = pointId;
}
